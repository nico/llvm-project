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

  // The symbol table.
  uint32_t NumSymbols = 0;
  uint64_t StringsSize = 1;
  for (uint32_t I = 0; I != Binary.NumSymbols; ++I) {
    MachOSymbol Symbol = Binary.symbol(I);
    if (!isKept(Symbol))
      continue;
    ++NumSymbols;
    if (!Symbol.Name.empty())
      StringsSize += Symbol.Name.size() + 1;
  }
  if (StringsSize > UINT32_MAX)
    fatal("the symbol names do not fit in a Mach-O string table");

  // The file: header and load commands, symbols, strings, DWARF.
  uint64_t SymbolsOffset =
      alignTo(sizeof(MachO::mach_header_64) + CommandsSize, 0x1000);
  uint64_t StringsOffset =
      SymbolsOffset + uint64_t(NumSymbols) * sizeof(MachO::nlist_64);
  uint64_t DwarfOffset = alignTo(StringsOffset + StringsSize, 0x1000);
  uint64_t DwarfSize = 0;
  for (Section &S : Sections) {
    S.FileOffset = DwarfOffset + DwarfSize;
    DwarfSize += S.Size;
  }

  SmallString<256> Path = BundlePath;
  sys::path::append(Path, "Contents", "Resources", "DWARF");
  if (std::error_code EC = sys::fs::create_directories(Path))
    fatal(Path + ": " + EC.message());
  writeInfoPlist(BundlePath, BinaryName);
  sys::path::append(Path, BinaryName);
  Expected<std::unique_ptr<FileOutputBuffer>> BufferOrError =
      FileOutputBuffer::create(Path, DwarfOffset + DwarfSize);
  if (!BufferOrError)
    fatal(Path + ": " + toString(BufferOrError.takeError()));
  Buffer = std::move(*BufferOrError);

  char *Start = reinterpret_cast<char *>(Buffer->getBufferStart());
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
  // exception is __LINKEDIT, which holds the symbol table.
  uint64_t EndAddress = 0;
  for (StringRef Command : Segments) {
    char *Segment = P;
    memcpy(P, Command.data(), Command.size());
    P += Command.size();
    uint64_t Address = read64le(Segment + 24);
    uint64_t FileOffset = 0, FileSize = 0;
    if (StringRef(Segment + 8, 16).rtrim('\0') == "__LINKEDIT") {
      FileOffset = SymbolsOffset;
      FileSize = StringsOffset + StringsSize - SymbolsOffset;
      write64le(Segment + 32, alignTo(FileSize, 0x1000)); // vmsize
    }
    write64le(Segment + 40, FileOffset);
    write64le(Segment + 48, FileSize);
    EndAddress = std::max(EndAddress, Address + read64le(Segment + 32));
    uint32_t NumSections = read32le(Segment + 64);
    for (uint32_t I = 0; I != NumSections; ++I) {
      char *Section = Segment + sizeof(MachO::segment_command_64) +
                      I * sizeof(MachO::section_64);
      write32le(Section + 48, 0); // offset
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
  for (const Section &S : Sections) {
    WriteName(S.Name);
    WriteName("__DWARF");
    Write64(DwarfAddress + (S.FileOffset - DwarfOffset));
    Write64(S.Size);
    // The offset field has 32 bits. Readers that support larger files
    // recover the rest from the order of the sections.
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
  P = Start + SymbolsOffset;
  char *Strings = Start + StringsOffset;
  uint64_t StringOffset = 0;
  Strings[StringOffset++] = '\0';
  for (uint32_t I = 0; I != Binary.NumSymbols; ++I) {
    MachOSymbol Symbol = Binary.symbol(I);
    if (!isKept(Symbol))
      continue;
    Write32(Symbol.Name.empty() ? 0 : StringOffset);
    *P++ = Symbol.Type;
    *P++ = Symbol.Section;
    write16le(P, Symbol.Desc);
    P += 2;
    Write64(Symbol.Value);
    if (!Symbol.Name.empty()) {
      memcpy(Strings + StringOffset, Symbol.Name.data(), Symbol.Name.size());
      StringOffset += Symbol.Name.size();
      Strings[StringOffset++] = '\0';
    }
  }
  assert(StringOffset == StringsSize);
  memset(Strings + StringsSize, 0, DwarfOffset - (StringsOffset + StringsSize));
}

void DsymWriter::commit() {
  if (Error E = Buffer->commit())
    fatal(Buffer->getPath() + ": " + toString(std::move(E)));
}
