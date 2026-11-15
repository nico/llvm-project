//===- ObjectFile.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An object file of the debug map: its DWARF sections, and the table that
// translates its addresses to addresses in the linked binary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_OBJECTFILE_H
#define LLVM_TOOLS_DSYMUTIL2_OBJECTFILE_H

#include "DebugMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class raw_ostream;

namespace dsymutil2 {

/// A symbol of the object file that made it into the linked binary.
struct AddressRange {
  /// In the object file. Variables have no known size: Begin == End. For a
  /// function, End is at first the end of the symbol, which includes the
  /// padding up to the next symbol; the scan moves it to the end of the
  /// function's code when it finds the function's debug info.
  uint64_t Begin;
  uint64_t End;
  /// What Begin became in the linked binary.
  uint64_t Linked;
};

/// A run of bytes of an object's __debug_info that goes to the output as it
/// is, except for the fields that get patched.
struct KeptRange {
  uint32_t InBegin;
  uint32_t InEnd;
  /// Where it goes, relative to the start of the object's contribution to
  /// the output's __debug_info.
  uint32_t Out;
};

/// A compile unit that is at least partly kept.
struct UnitInfo {
  /// Of the unit header, in the object's __debug_info.
  uint32_t InOffset;
  /// Of its abbreviation table, in the object's __debug_abbrev.
  uint32_t AbbrevOffset;
  uint8_t Version;
  uint8_t AddressSize;
  uint8_t HeaderSize;
  /// Its ranges in ObjectFile::KeptRanges.
  uint32_t FirstRange;
  uint32_t NumRanges;
  /// Of the unit in the object's contribution, and its size with header.
  uint32_t OutOffset;
  uint32_t OutSize;
  /// Of its line table in ObjectFile::LineData, if it has one.
  uint32_t LineOffset = 0;
  bool HasLineTable = false;
  /// The linked address ranges of its functions that are in the binary, in
  /// ObjectFile::UnitRanges: sorted and not overlapping.
  uint32_t FirstAddressRange = 0;
  uint32_t NumAddressRanges = 0;
  /// The unit's base address in the object file, and whether the unit has
  /// one in the output. Range lists are relative to it.
  uint64_t InLowPc = 0;
  bool HasLowPc = false;
  /// The first of its range lists in ObjectFile::RangeListOffsets.
  uint32_t FirstRangeList = 0;
};

struct ObjectFile {
  const MapObject *Map = nullptr;
  /// Keeps the file mapped, for plain object files and thin archive members.
  std::unique_ptr<MemoryBuffer> Buffer;
  /// Set if the object could not be used; the driver prints it.
  std::string Warning;

  /// DWARF sections, empty if absent.
  StringRef Info, Abbrev, Str, Line, Ranges, Frame;

  /// For each symbol of Map, its address in this file, or NotFound.
  std::vector<uint64_t> SymbolAddresses;
  static constexpr uint64_t NotFound = ~uint64_t(0);

  /// The found symbols, sorted by Begin.
  std::vector<AddressRange> Addresses;

  /// What the scan decided to keep. KeptRanges is sorted by InBegin.
  std::vector<UnitInfo> Units;
  std::vector<KeptRange> KeptRanges;
  /// The size of this object's contribution to the output's __debug_info.
  uint64_t InfoSize = 0;
  /// The strings that the kept entries use: their offsets in Str, sorted,
  /// and for each of them its offset in the output's __debug_str.
  std::vector<uint32_t> StringOffsets;
  std::vector<uint32_t> StringOutOffsets;
  /// For each of them, used by the string merging: first the string's cell
  /// in the hash table, then whether this object writes the string.
  std::vector<uint32_t> StringCells;
  /// The size of the strings this object writes.
  uint64_t StrSize = 0;
  /// This object's contributions to the output's __debug_line and
  /// __debug_ranges.
  SmallVector<char, 0> LineData;
  SmallVector<char, 0> RangesData;
  /// The linked address ranges of the units' functions: begin and end.
  std::vector<std::pair<uint64_t, uint64_t>> UnitRanges;
  /// For each DW_AT_ranges of a kept entry, in the order of the entries,
  /// the offset of its list in RangesData.
  std::vector<uint32_t> RangeListOffsets;
  /// The size of this object's contribution to __debug_aranges.
  uint64_t ArangesSize = 0;
  /// The frame descriptions (FDEs) of the functions that are in the binary.
  /// Until they are written, each names its CIE by its index in FrameCIEs.
  SmallVector<char, 0> FrameData;
  /// The distinct CIEs that those use, in the object's __debug_frame, and
  /// where each is in the output's.
  std::vector<StringRef> FrameCIEs;
  std::vector<uint32_t> FrameCIEOffsets;
  /// Where this object's contributions start in the output's sections.
  uint64_t InfoOffset = 0;
  uint64_t AbbrevOffset = 0;
  uint64_t LineOffset = 0;
  uint64_t RangesOffset = 0;
  uint64_t ArangesOffset = 0;
  /// In __debug_frame, the object's contribution is the CIEs that it is the
  /// first to use, then its FDEs.
  uint64_t FrameOffset = 0;
  uint64_t FrameFDEOffset = 0;
  uint64_t StrOffset = 0;

  /// The offset in the output's __debug_str of the string at \p Offset of
  /// Str, which must be in StringOffsets.
  uint32_t stringOutOffset(uint32_t Offset) const {
    auto It = llvm::lower_bound(StringOffsets, Offset);
    return StringOutOffsets[It - StringOffsets.begin()];
  }

  /// The range that starts at \p Address, or null.
  const AddressRange *findStart(uint64_t Address) const;
  /// The range that contains \p Address, or null.
  const AddressRange *findContaining(uint64_t Address) const;
  AddressRange *findStart(uint64_t Address) {
    return const_cast<AddressRange *>(std::as_const(*this).findStart(Address));
  }
};

struct InputFiles {
  /// One per object of the debug map, in its order.
  std::vector<ObjectFile> Objects;
  /// The regular archives that members among Objects point into.
  std::vector<std::unique_ptr<MemoryBuffer>> Archives;
};

/// Opens every object file of \p Map and matches the debug map's symbols
/// against the object files' symbol tables. \p PrependPath is put in front of
/// the paths of the debug map.
InputFiles openObjectFiles(const DebugMap &Map, StringRef PrependPath);

void dumpDebugMap(const std::vector<ObjectFile> &Objects, raw_ostream &OS);

} // namespace dsymutil2
} // namespace llvm

#endif
