//===- MachOFile.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A minimal reader for thin 64-bit little-endian Mach-O files: load commands,
// sections and the symbol table. This tool opens tens of thousands of object
// files and needs nothing else from them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_MACHOFILE_H
#define LLVM_TOOLS_DSYMUTIL2_MACHOFILE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include <string>
#include <vector>

namespace llvm {
namespace dsymutil2 {

struct MachOSection {
  StringRef Segment;
  StringRef Name;
  uint64_t Address;
  uint64_t Size;
  uint32_t Offset;
  uint32_t Flags;
  /// Where the section's relocations are in the file, and how many.
  uint32_t RelocationsOffset;
  uint32_t NumRelocations;
};

/// An entry of the symbol table (a nlist_64).
struct MachOSymbol {
  StringRef Name;
  uint8_t Type;
  uint8_t Section;
  uint16_t Desc;
  uint64_t Value;

  bool isStab() const { return Type & MachO::N_STAB; }
  bool isDefinedInSection() const {
    return !isStab() && (Type & MachO::N_TYPE) == MachO::N_SECT;
  }
};

class MachOFile {
public:
  /// Parses \p Data. Returns an empty string on success and a description of
  /// the problem otherwise.
  std::string parse(StringRef Data);

  MachOSymbol symbol(uint32_t Index) const {
    const char *Entry = Symbols + uint64_t(Index) * sizeof(MachO::nlist_64);
    // The name ends at the first NUL, or at the end of the string table.
    StringRef Name = Strings.substr(support::endian::read32le(Entry));
    MachOSymbol Symbol = symbolWithoutName(Index);
    Symbol.Name = Name.substr(0, Name.find('\0'));
    return Symbol;
  }

  /// The same with an empty name: finding where a name ends takes a look at
  /// the string table, which is much larger than the symbols.
  MachOSymbol symbolWithoutName(uint32_t Index) const {
    const char *Entry = Symbols + uint64_t(Index) * sizeof(MachO::nlist_64);
    return {StringRef(), uint8_t(Entry[4]), uint8_t(Entry[5]),
            support::endian::read16le(Entry + 6),
            support::endian::read64le(Entry + 8)};
  }

  /// The contents of section \p Name in segment \p Segment, empty if there is
  /// none.
  StringRef sectionContents(StringRef Segment, StringRef Name) const;

  /// The offsets in that section of the fields that the linker is to add the
  /// address of a symbol to, as opposed to that of a section: only then does
  /// the field not hold the address of what it refers to in this file.
  std::vector<uint32_t> externRelocations(StringRef Segment,
                                          StringRef Name) const;

  StringRef Data;
  uint32_t CPUType = 0;
  uint32_t CPUSubtype = 0;
  uint32_t FileType = 0;
  /// Each load command's bytes, in file order.
  std::vector<StringRef> LoadCommands;
  /// All sections of all segments, in file order. Symbols refer to them by
  /// index + 1.
  std::vector<MachOSection> Sections;
  const char *Symbols = nullptr;
  uint32_t NumSymbols = 0;
  StringRef Strings;
};

} // namespace dsymutil2
} // namespace llvm

#endif
