//===- DsymWriter.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DsymWriter.h"
#include "MachOFile.h"
#include "dsymutil2.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

static void writeInfoPlist(StringRef BundlePath, StringRef BinaryName) {
  SmallString<256> Path = BundlePath;
  sys::path::append(Path, "Contents", "Info.plist");
  std::error_code EC;
  raw_fd_ostream OS(Path, EC);
  if (EC)
    fatal(Path + ": " + EC.message());
  OS << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     << "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
     << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
     << "<plist version=\"1.0\">\n"
     << "\t<dict>\n"
     << "\t\t<key>CFBundleDevelopmentRegion</key>\n"
     << "\t\t<string>English</string>\n"
     << "\t\t<key>CFBundleIdentifier</key>\n"
     << "\t\t<string>com.apple.xcode.dsym.";
  printHTMLEscaped(BinaryName, OS);
  OS << "</string>\n"
     << "\t\t<key>CFBundleInfoDictionaryVersion</key>\n"
     << "\t\t<string>6.0</string>\n"
     << "\t\t<key>CFBundlePackageType</key>\n"
     << "\t\t<string>dSYM</string>\n"
     << "\t\t<key>CFBundleSignature</key>\n"
     << "\t\t<string>\?\?\?\?</string>\n"
     << "\t\t<key>CFBundleShortVersionString</key>\n"
     << "\t\t<string>1.0</string>\n"
     << "\t\t<key>CFBundleVersion</key>\n"
     << "\t\t<string>1</string>\n"
     << "\t</dict>\n"
     << "</plist>\n";
}

/// Whether the dSYM gets a copy of \p Symbol. The debug map and undefined
/// symbols are of no use there.
static bool isKept(const MachOSymbol &Symbol) {
  return !Symbol.isStab() && (Symbol.Type & MachO::N_TYPE) != MachO::N_UNDF;
}

void DsymWriter::create(StringRef BundlePath, StringRef BinaryName) {
  // The load commands: the binary's UUID and build version, a symbol table,
  // the binary's segments and the __DWARF segment.
  std::vector<StringRef> CopiedCommands;
  std::vector<StringRef> Segments;
  for (StringRef Command : Binary.LoadCommands) {
    uint32_t Kind = read32le(Command.data());
    if (Kind == MachO::LC_UUID || Kind == MachO::LC_BUILD_VERSION ||
        Kind == MachO::LC_VERSION_MIN_MACOSX ||
        Kind == MachO::LC_VERSION_MIN_IPHONEOS ||
        Kind == MachO::LC_VERSION_MIN_TVOS ||
        Kind == MachO::LC_VERSION_MIN_WATCHOS)
      CopiedCommands.push_back(Command);
    else if (Kind == MachO::LC_SEGMENT_64 &&
             StringRef(Command.data() + 8, 7) != "__DWARF")
      Segments.push_back(Command);
  }
  uint64_t CommandsSize =
      sizeof(MachO::symtab_command) + sizeof(MachO::segment_command_64) +
      Sections.size() * sizeof(MachO::section_64);
  for (StringRef Command : CopiedCommands)
    CommandsSize += Command.size();
  for (StringRef Command : Segments)
    CommandsSize += Command.size();

  // The symbol table. Runs of the binary's symbols are copied in parallel;
  // this finds where the copy of each run starts.
  constexpr uint32_t RunSize = 1 << 15;
  struct SymbolRun {
    /// Of the kept symbols before this run.
    uint64_t NumSymbols = 0;
    uint64_t StringsSize = 0;
  };
  size_t NumRuns = divideCeil(Binary.NumSymbols, RunSize);
  std::vector<SymbolRun> Runs(NumRuns + 1);
  parallelFor(0, NumRuns, [&](size_t R) {
    SymbolRun &Next = Runs[R + 1];
    uint32_t End =
        std::min<uint64_t>(uint64_t(R + 1) * RunSize, Binary.NumSymbols);
    for (uint32_t I = R * RunSize; I != End; ++I) {
      MachOSymbol Symbol = Binary.symbol(I);
      if (!isKept(Symbol))
        continue;
      ++Next.NumSymbols;
      if (!Symbol.Name.empty())
        Next.StringsSize += Symbol.Name.size() + 1;
    }
  });
  // The string table starts with an empty string.
  Runs[0].StringsSize = 1;
  for (size_t R = 0; R != NumRuns; ++R) {
    Runs[R + 1].NumSymbols += Runs[R].NumSymbols;
    Runs[R + 1].StringsSize += Runs[R].StringsSize;
  }
  uint64_t NumSymbols = Runs[NumRuns].NumSymbols;
  uint64_t StringsSize = Runs[NumRuns].StringsSize;

  // The file: header and load commands, symbols, strings, the binary's
  // __eh_frame, DWARF. Functions that unwind with __eh_frame have no entry in
  // __debug_frame, and consumers of the dSYM look for both there.
  StringRef EHFrame = Binary.sectionContents("__TEXT", "__eh_frame");
  uint64_t SymbolsOffset =
      alignTo(sizeof(MachO::mach_header_64) + CommandsSize, 0x1000);
  uint64_t StringsOffset = SymbolsOffset + NumSymbols * sizeof(MachO::nlist_64);
  uint64_t EHFrameOffset = alignTo(StringsOffset + StringsSize, 0x1000);
  uint64_t DwarfOffset = alignTo(EHFrameOffset + EHFrame.size(), 0x1000);
  // What is before the DWARF has file offsets of 32 bits that no reader can
  // make up for.
  if (DwarfOffset > UINT32_MAX)
    fatal("the symbol table does not fit in 4 GB");
  // The sections in the order in which they are in the file.
  std::vector<unsigned> Order;
  uint64_t DwarfSize = 0;
  auto Place = [&](unsigned Index) {
    Section &S = Sections[Index];
    S.FileOffset = DwarfOffset + DwarfSize;
    // The section's header has 32 bits for this; see there. Like dsymutil,
    // write such a file only on request.
    if (S.FileOffset >= OffsetLimit && !AllowOffsetOverflow)
      fatal("section " + S.Name +
            " starts past 4 GB in the dSYM, which not every reader can "
            "handle; write it with --allow-section-header-offset-overflow");
    DwarfSize += S.Size;
    Order.push_back(Index);
  };
  std::optional<unsigned> Late = LastReachable;
  for (unsigned I = 0; I != Sections.size(); ++I) {
    if (LastReachable == I)
      continue;
    // With this section in front of it, the late one would start too late.
    if (Late && DwarfOffset + DwarfSize + Sections[I].Size >= OffsetLimit)
      Place(*std::exchange(Late, std::nullopt));
    Place(I);
  }
  if (Late)
    Place(*Late);

  SmallString<256> Directory = BundlePath;
  sys::path::append(Directory, "Contents", "Resources", "DWARF");
  if (std::error_code EC = sys::fs::create_directories(Directory))
    fatal(Directory + ": " + EC.message());
  writeInfoPlist(BundlePath, BinaryName);
  sys::path::append(Directory, BinaryName);
  Path = std::string(Directory);
  Expected<sys::fs::TempFile> FileOrError =
      sys::fs::TempFile::create(Path + ".tmp%%%%%%%");
  if (!FileOrError)
    fatal(Path + ": " + toString(FileOrError.takeError()));
  File.emplace(std::move(*FileOrError));

  // The file is built in memory and not in a mapping of the file: on macOS,
  // filling a mapping takes twice as long as filling memory, and handing the
  // pages to the file system at the end takes as long as writing the memory
  // to the file.
  for (Section &S : Sections) {
    if (S.Data)
      continue;
    S.Buffer.reset(new char[S.Size]);
    S.Data = S.Buffer.get();
  }
  HeadSize = DwarfOffset;
  Head.reset(new char[HeadSize]);
  char *Start = Head.get();
  char *P = Start;
  auto Write32 = [&](uint32_t Value) {
    write32le(P, Value);
    P += 4;
  };
  auto Write64 = [&](uint64_t Value) {
    write64le(P, Value);
    P += 8;
  };
  auto WriteName = [&](StringRef Name) {
    memset(P, 0, 16);
    memcpy(P, Name.data(), std::min<size_t>(Name.size(), 16));
    P += 16;
  };

  Write32(MachO::MH_MAGIC_64);
  Write32(Binary.CPUType);
  Write32(Binary.CPUSubtype);
  Write32(MachO::MH_DSYM);
  Write32(CopiedCommands.size() + 1 + Segments.size() + 1);
  Write32(CommandsSize);
  Write32(0); // flags
  Write32(0); // reserved

  for (StringRef Command : CopiedCommands) {
    memcpy(P, Command.data(), Command.size());
    P += Command.size();
  }

  Write32(MachO::LC_SYMTAB);
  Write32(sizeof(MachO::symtab_command));
  Write32(SymbolsOffset);
  Write32(NumSymbols);
  Write32(StringsOffset);
  Write32(StringsSize);

  // The binary's segments keep their addresses, so that the symbols and the
  // DWARF can be resolved against them, but have no contents here. The
  // exceptions are __LINKEDIT, which holds the symbol table, and __TEXT if
  // it has an __eh_frame section.
  uint64_t EndAddress = 0;
  for (StringRef Command : Segments) {
    char *Segment = P;
    memcpy(P, Command.data(), Command.size());
    P += Command.size();
    uint64_t Address = read64le(Segment + 24);
    uint64_t FileOffset = 0, FileSize = 0;
    StringRef SegmentName = StringRef(Segment + 8, 16).rtrim('\0');
    if (SegmentName == "__LINKEDIT") {
      FileOffset = SymbolsOffset;
      FileSize = StringsOffset + StringsSize - SymbolsOffset;
      write64le(Segment + 32, alignTo(FileSize, 0x1000)); // vmsize
    } else if (SegmentName == "__TEXT" && !EHFrame.empty()) {
      FileOffset = EHFrameOffset;
      FileSize = EHFrame.size();
    }
    write64le(Segment + 40, FileOffset);
    write64le(Segment + 48, FileSize);
    EndAddress = std::max(EndAddress, Address + read64le(Segment + 32));
    uint32_t NumSections = read32le(Segment + 64);
    for (uint32_t I = 0; I != NumSections; ++I) {
      char *Section = Segment + sizeof(MachO::segment_command_64) +
                      I * sizeof(MachO::section_64);
      bool IsEHFrame = SegmentName == "__TEXT" && !EHFrame.empty() &&
                       StringRef(Section, 16).rtrim('\0') == "__eh_frame";
      write32le(Section + 48, IsEHFrame ? EHFrameOffset : 0); // offset
      write32le(Section + 56, 0); // reloff
      write32le(Section + 60, 0); // nreloc
    }
  }

  uint64_t DwarfAddress = alignTo(EndAddress, 0x1000);
  Write32(MachO::LC_SEGMENT_64);
  Write32(sizeof(MachO::segment_command_64) +
          Sections.size() * sizeof(MachO::section_64));
  WriteName("__DWARF");
  Write64(DwarfAddress);
  Write64(alignTo(DwarfSize, 0x1000));
  Write64(DwarfOffset);
  Write64(DwarfSize);
  Write32(MachO::VM_PROT_READ | MachO::VM_PROT_WRITE | MachO::VM_PROT_EXECUTE);
  Write32(MachO::VM_PROT_READ | MachO::VM_PROT_WRITE);
  Write32(Sections.size());
  Write32(0); // flags
  for (unsigned Index : Order) {
    const Section &S = Sections[Index];
    WriteName(S.Name);
    WriteName("__DWARF");
    Write64(DwarfAddress + (S.FileOffset - DwarfOffset));
    Write64(S.Size);
    // The offset field has 32 bits. Readers that support larger files
    // recover the rest: LLVM and lldb from the order of the sections, which
    // are in the file in the order of their headers and without gaps, and
    // Breakpad from the section's address, which is as far from the
    // segment's address as the section is from the segment's start in the
    // file.
    Write32(S.FileOffset);
    Write32(0); // align
    Write32(0); // reloff
    Write32(0); // nreloc
    Write32(MachO::S_ATTR_DEBUG);
    Write32(0); // reserved1
    Write32(0); // reserved2
    Write32(0); // reserved3
  }
  assert(P == Start + sizeof(MachO::mach_header_64) + CommandsSize);
  memset(P, 0, Start + SymbolsOffset - P);

  // The symbols, with a new string table.
  char *Strings = Start + StringsOffset;
  Strings[0] = '\0';
  parallelFor(0, NumRuns, [&](size_t R) {
    char *P =
        Start + SymbolsOffset + Runs[R].NumSymbols * sizeof(MachO::nlist_64);
    uint64_t StringOffset = Runs[R].StringsSize;
    uint32_t End =
        std::min<uint64_t>(uint64_t(R + 1) * RunSize, Binary.NumSymbols);
    for (uint32_t I = R * RunSize; I != End; ++I) {
      MachOSymbol Symbol = Binary.symbol(I);
      if (!isKept(Symbol))
        continue;
      write32le(P, Symbol.Name.empty() ? 0 : StringOffset);
      P[4] = Symbol.Type;
      P[5] = Symbol.Section;
      write16le(P + 6, Symbol.Desc);
      write64le(P + 8, Symbol.Value);
      P += sizeof(MachO::nlist_64);
      if (!Symbol.Name.empty()) {
        memcpy(Strings + StringOffset, Symbol.Name.data(), Symbol.Name.size());
        StringOffset += Symbol.Name.size();
        Strings[StringOffset++] = '\0';
      }
    }
    assert(StringOffset == Runs[R + 1].StringsSize);
  });
  memset(Strings + StringsSize, 0,
         EHFrameOffset - (StringsOffset + StringsSize));
  memcpy(Start + EHFrameOffset, EHFrame.data(), EHFrame.size());
  memset(Start + EHFrameOffset + EHFrame.size(), 0,
         DwarfOffset - (EHFrameOffset + EHFrame.size()));

  // Without threads, the file is written in commit().
#if LLVM_ENABLE_THREADS
  if (parallel::strategy.ThreadsRequested != 1)
    Thread = std::thread([this] { writeFile(); });
#endif
}

/// Writes the file: each section when it is complete, at its place.
void DsymWriter::writeFile() {
  raw_fd_ostream OS(File->FD, /*shouldClose=*/false, /*unbuffered=*/true);
  OS.write(Head.get(), HeadSize);
  for (size_t NumWritten = 0; NumWritten != Sections.size(); ++NumWritten) {
    auto Next = Sections.end();
    {
      std::unique_lock<std::mutex> Lock(Mutex);
      SectionFinished.wait(Lock, [&] {
        Next = llvm::find_if(Sections, [](const Section &S) {
          return S.IsComplete && !S.IsWritten;
        });
        return Next != Sections.end();
      });
      Next->IsWritten = true;
    }
    OS.seek(Next->FileOffset);
    OS.write(Next->Data, Next->Size);
  }
  WriteError = OS.error();
  OS.clear_error();
}

void DsymWriter::finishSection(unsigned Index) {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    Sections[Index].IsComplete = true;
  }
  SectionFinished.notify_one();
}

void DsymWriter::commit() {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    for (Section &S : Sections)
      S.IsComplete = true;
  }
  SectionFinished.notify_one();
  if (Thread.joinable())
    Thread.join();
  else
    writeFile();
  if (WriteError)
    fatal(Path + ": " + WriteError.message());
  if (Error E = File->keep(Path))
    fatal(Path + ": " + toString(std::move(E)));
}
