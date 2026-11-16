//===- MachOFile.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MachOFile.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

/// A fixed-size name field: up to 16 characters, NUL-padded.
static StringRef fixedName(const char *Field) {
  return StringRef(Field, strnlen(Field, 16));
}

std::string MachOFile::parse(StringRef Contents) {
  Data = Contents;
  auto InBounds = [&](uint64_t Offset, uint64_t Size) {
    return Offset <= Data.size() && Size <= Data.size() - Offset;
  };
  if (!InBounds(0, sizeof(MachO::mach_header_64)))
    return "truncated or malformed Mach-O file";
  uint32_t Magic = read32le(Data.data());
  if (Magic == MachO::FAT_MAGIC || Magic == MachO::FAT_CIGAM ||
      Magic == MachO::FAT_MAGIC_64 || Magic == MachO::FAT_CIGAM_64)
    return "universal binaries are not supported";
  if (Magic != MachO::MH_MAGIC_64)
    return "not a 64-bit little-endian Mach-O file";
  CPUType = read32le(Data.data() + 4);
  CPUSubtype = read32le(Data.data() + 8);
  FileType = read32le(Data.data() + 12);
  uint32_t NumCommands = read32le(Data.data() + 16);

  uint64_t Offset = sizeof(MachO::mach_header_64);
  for (uint32_t I = 0; I != NumCommands; ++I) {
    if (!InBounds(Offset, sizeof(MachO::load_command)))
      return "truncated or malformed Mach-O file";
    const char *Command = Data.data() + Offset;
    uint32_t Kind = read32le(Command);
    uint32_t Size = read32le(Command + 4);
    if (Size < sizeof(MachO::load_command) || !InBounds(Offset, Size))
      return "malformed load command";
    LoadCommands.push_back(Data.substr(Offset, Size));
    Offset += Size;

    if (Kind == MachO::LC_SYMTAB) {
      if (Size < sizeof(MachO::symtab_command))
        return "malformed symbol table load command";
      uint32_t SymbolsOffset = read32le(Command + 8);
      NumSymbols = read32le(Command + 12);
      uint32_t StringsOffset = read32le(Command + 16);
      uint32_t StringsSize = read32le(Command + 20);
      if (!InBounds(SymbolsOffset,
                    uint64_t(NumSymbols) * sizeof(MachO::nlist_64)) ||
          !InBounds(StringsOffset, StringsSize))
        return "symbol table extends past the end of the file";
      Symbols = Data.data() + SymbolsOffset;
      Strings = Data.substr(StringsOffset, StringsSize);
    } else if (Kind == MachO::LC_SEGMENT_64) {
      if (Size < sizeof(MachO::segment_command_64))
        return "malformed segment load command";
      uint32_t NumSections = read32le(Command + 64);
      if (uint64_t(NumSections) * sizeof(MachO::section_64) >
          Size - sizeof(MachO::segment_command_64))
        return "malformed segment load command";
      const char *Section = Command + sizeof(MachO::segment_command_64);
      for (uint32_t J = 0; J != NumSections; ++J) {
        Sections.push_back({fixedName(Section + 16), fixedName(Section),
                            read64le(Section + 32), read64le(Section + 40),
                            read32le(Section + 48), read32le(Section + 64),
                            read32le(Section + 56), read32le(Section + 60)});
        Section += sizeof(MachO::section_64);
      }
    }
  }
  return "";
}

StringRef MachOFile::sectionContents(StringRef Segment, StringRef Name) const {
  for (const MachOSection &Section : Sections) {
    if (Section.Name != Name || Section.Segment != Segment)
      continue;
    uint8_t Type = Section.Flags & MachO::SECTION_TYPE;
    if (Type == MachO::S_ZEROFILL || Section.Offset > Data.size() ||
        Section.Size > Data.size() - Section.Offset)
      return StringRef();
    return Data.substr(Section.Offset, Section.Size);
  }
  return StringRef();
}

std::vector<ExternRelocation>
MachOFile::externRelocations(StringRef Segment, StringRef Name) const {
  std::vector<ExternRelocation> Relocations;
  for (const MachOSection &Section : Sections) {
    if (Section.Name != Name || Section.Segment != Segment)
      continue;
    uint64_t Size =
        uint64_t(Section.NumRelocations) * sizeof(MachO::any_relocation_info);
    if (Section.RelocationsOffset > Data.size() ||
        Size > Data.size() - Section.RelocationsOffset)
      break;
    const char *Relocation = Data.data() + Section.RelocationsOffset;
    for (uint32_t I = 0; I != Section.NumRelocations; ++I) {
      // The second word has the symbol's index in its low 24 bits and
      // r_extern in bit 27. Scattered relocations, which have the highest bit
      // of the first word set, are not for 64-bit files.
      uint32_t Word = read32le(Relocation + 4);
      if (Word & (1u << 27))
        Relocations.push_back({read32le(Relocation), Word & 0xffffff});
      Relocation += sizeof(MachO::any_relocation_info);
    }
    break;
  }
  llvm::sort(Relocations,
             [](const ExternRelocation &A, const ExternRelocation &B) {
               return A.Offset < B.Offset;
             });
  return Relocations;
}
