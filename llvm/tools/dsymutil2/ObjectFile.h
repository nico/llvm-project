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
#include "llvm/ADT/bit.h"
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

/// A table of an object file that the entries of its units index into, like
/// the table of the strings' offsets of DWARF 5, and which of its slots the
/// kept entries use. The output's table has only those, in the same order.
/// An index only gets smaller that way, so that it fits where the old one
/// was, and the entry that it is in keeps its size.
class IndexTable {
public:
  IndexTable(uint32_t Base, unsigned SlotSize)
      : Base(Base), SlotSize(SlotSize) {}

  /// Where slot 0 is in the object's section. Units name their table by
  /// this.
  uint32_t Base;
  /// The number of bytes of a slot.
  uint32_t SlotSize;
  /// Where the first used slot is in the object's contribution to the
  /// output's section.
  uint32_t OutBase = 0;

  void use(uint32_t Slot) {
    if (Slot / 64 >= Used.size())
      Used.resize(Slot / 64 + 1);
    Used[Slot / 64] |= uint64_t(1) << (Slot % 64);
  }
  /// Has to follow the last use().
  void finish();
  /// The number of used slots.
  uint32_t size() const { return NumUsed; }
  /// The index in the output's table of the used slot \p Slot.
  uint32_t outIndex(uint32_t Slot) const {
    uint64_t Before = Used[Slot / 64] & ((uint64_t(1) << (Slot % 64)) - 1);
    return Ranks[Slot / 64] + llvm::popcount(Before);
  }
  /// Calls \p OnSlot(Slot) for the used slots, in order.
  template <class FnTy> void forEachSlot(FnTy OnSlot) const {
    for (size_t I = 0; I != Used.size(); ++I)
      for (uint64_t Word = Used[I]; Word; Word &= Word - 1)
        OnSlot(uint32_t(I * 64 + llvm::countr_zero(Word)));
  }

private:
  /// A bit per slot.
  std::vector<uint64_t> Used;
  /// For every 64 slots, the number of used ones before them.
  std::vector<uint32_t> Ranks;
  uint32_t NumUsed = 0;
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
  /// DWARF 5: the bases of its tables in ObjectFile::StrTables and
  /// ObjectFile::AddrTables.
  uint32_t StrOffsetsBase = 0;
  uint32_t AddrBase = 0;
  /// DWARF 5: if the DW_AT_low_pc of the unit's own entry is an index into
  /// the table of addresses, where that index is in Info, its size, and
  /// where its form is in Abbrev. The output has the address there instead,
  /// with the form DW_FORM_addr: the slot has the address of the unit's
  /// first function, and the unit's code can start with another one in the
  /// binary. It is the one value of an entry that changes its size.
  uint32_t LowPcOffset = 0;
  uint32_t LowPcFormOffset = 0;
  uint8_t LowPcSize = 0;
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
  /// The first of its range lists in ObjectFile::RangeListOffsets, and of
  /// its location lists in ObjectFile::LocListOffsets.
  uint32_t FirstRangeList = 0;
  uint32_t FirstLocList = 0;
};

/// Says that references to an entry of this object file go to another copy
/// of the entry instead: the one in the object file that owns it.
struct Redirect {
  /// The entry in this object's __debug_info.
  uint32_t InOffset;
  /// The index of the owning object file, and the entry in its __debug_info.
  uint32_t OwnerObject;
  uint32_t OwnerOffset;
  /// Where the owner's copy is in the output's __debug_info. Known after the
  /// layout.
  uint32_t Out = 0;
};

/// A name that an entry can be found by: an element of an accelerator table.
struct AccelName {
  /// The offset of the name in the object's strings.
  uint32_t Name;
  /// The offset of the entry in the object's contribution to the output's
  /// __debug_info.
  uint32_t Entry;
};

/// The same for a type. The table of types says more about its entries.
struct AccelType {
  uint32_t Name;
  uint32_t Entry;
  /// The hash of the name together with the names of what the type is in.
  uint32_t QualifiedNameHash;
  uint16_t Tag;
  /// Whether this is the implementation of an Objective-C class.
  bool IsObjCImplementation;
};

struct ObjectFile {
  const MapObject *Map = nullptr;
  /// Keeps the file mapped, for plain object files and thin archive members.
  std::unique_ptr<MemoryBuffer> Buffer;
  /// Set if the object could not be used; the driver prints it.
  std::string Warning;

  /// DWARF sections, empty if absent.
  StringRef Info, Abbrev, Str, Line, Ranges, Loc, Frame;
  /// Those that only DWARF 5 has.
  StringRef StrOffsets, Addr;

  /// For each symbol of Map, its address in this file, or NotFound.
  std::vector<uint64_t> SymbolAddresses;
  static constexpr uint64_t NotFound = ~uint64_t(0);

  /// The found symbols, sorted by Begin.
  std::vector<AddressRange> Addresses;
  /// The addresses in a section that are not addresses of this file: the
  /// linker was to make them those of symbols that the file has no address
  /// for.
  struct ExternAddresses {
    /// Where they are in the section, sorted.
    std::vector<uint32_t> Offsets;
    /// For each of them, where the symbol is in the binary, as a range that
    /// starts at 0, if it is a common symbol of this file that the debug map
    /// has: a C variable without an initial value has no place in the object
    /// file. Else Linked is NotFound: another file defines the symbol.
    std::vector<AddressRange> Ranges;
  };
  /// Those in Info and in Addr.
  ExternAddresses InfoExterns, AddrExterns;

  /// What the scan decided to keep. KeptRanges is sorted by InBegin.
  std::vector<UnitInfo> Units;
  std::vector<KeptRange> KeptRanges;
  /// The size of this object's contribution to the output's __debug_info.
  uint64_t InfoSize = 0;
  /// The entries that are not kept here because another object file owns
  /// them, sorted by InOffset.
  std::vector<Redirect> Redirects;
  /// Sorted offsets in Abbrev of forms that are DW_FORM_ref4 there and have
  /// to be DW_FORM_ref_addr in the output, because an attribute with that
  /// form refers to a redirected entry.
  std::vector<uint32_t> RefAddrForms;
  /// Strings that are not in Str but have to be in the output's __debug_str:
  /// names that the accelerator tables list and that the debug info does not
  /// spell out, like the name of a function without its template arguments.
  /// Each ends in a 0. Their offsets continue those of Str: the first one is
  /// at offset Str.size().
  SmallVector<char, 0> ExtraStrings;
  /// The strings that the kept entries and the accelerator tables use: their
  /// offsets, sorted, and for each of them its offset in the output's
  /// __debug_str.
  std::vector<uint32_t> StringOffsets;
  std::vector<uint32_t> StringOutOffsets;
  /// The tables of __debug_str_offs that the kept units use: a slot has the
  /// offset of a string in Str. Compilers make one table for all units of an
  /// object file.
  std::vector<IndexTable> StrTables;
  /// The same for __debug_addr, whose slots are addresses.
  std::vector<IndexTable> AddrTables;
  /// The sizes of this object's contributions to the output's
  /// __debug_str_offs and __debug_addr.
  uint64_t StrOffsetsSize = 0;
  uint64_t AddrSize = 0;
  /// For each of them, used by the string merging: first the string's cell
  /// in the hash table, then whether this object writes the string.
  std::vector<uint32_t> StringCells;
  /// The size of the strings this object writes.
  uint64_t StrSize = 0;
  /// This object's contributions to the output's __debug_line,
  /// __debug_ranges and __debug_loc.
  SmallVector<char, 0> LineData;
  SmallVector<char, 0> RangesData;
  SmallVector<char, 0> LocData;
  /// The linked address ranges of the units' functions: begin and end.
  std::vector<std::pair<uint64_t, uint64_t>> UnitRanges;
  /// For each DW_AT_ranges of a kept entry, in the order of the entries,
  /// the offset of its list in RangesData.
  std::vector<uint32_t> RangeListOffsets;
  /// The same for the location lists of the kept entries and LocData.
  std::vector<uint32_t> LocListOffsets;
  /// The size of this object's contribution to __debug_aranges.
  uint64_t ArangesSize = 0;
  /// The frame descriptions (FDEs) of the functions that are in the binary.
  /// Until they are written, each names its CIE by its index in FrameCIEs.
  SmallVector<char, 0> FrameData;
  /// The distinct CIEs that those use, in the object's __debug_frame, and
  /// where each is in the output's.
  std::vector<StringRef> FrameCIEs;
  std::vector<uint32_t> FrameCIEOffsets;
  /// The names that the kept entries can be found by, in the order of the
  /// entries. Each list goes into one accelerator table.
  std::vector<AccelName> Names;
  std::vector<AccelName> Namespaces;
  std::vector<AccelName> ObjCClasses;
  std::vector<AccelType> Types;
  /// Where this object's contributions start in the output's sections.
  uint64_t InfoOffset = 0;
  uint64_t AbbrevOffset = 0;
  uint64_t LineOffset = 0;
  uint64_t RangesOffset = 0;
  uint64_t LocOffset = 0;
  uint64_t ArangesOffset = 0;
  /// In __debug_frame, the object's contribution is the CIEs that it is the
  /// first to use, then its FDEs.
  uint64_t FrameOffset = 0;
  uint64_t FrameFDEOffset = 0;
  uint64_t StrOffset = 0;
  uint64_t StrOffsetsOffset = 0;
  uint64_t AddrOffset = 0;

  /// The string at \p Offset of Str, or of ExtraStrings past the end of Str.
  StringRef string(uint32_t Offset) const {
    StringRef S = Offset < Str.size()
                      ? Str.substr(Offset)
                      : StringRef(ExtraStrings.data(), ExtraStrings.size())
                            .substr(Offset - Str.size());
    return S.substr(0, S.find('\0'));
  }

  /// The offset in the output's __debug_str of the string at \p Offset,
  /// which must be in StringOffsets.
  uint32_t stringOutOffset(uint32_t Offset) const {
    auto It = llvm::lower_bound(StringOffsets, Offset);
    return StringOutOffsets[It - StringOffsets.begin()];
  }

  /// The table of \p Tables whose slot 0 is at \p Base of its section. If
  /// there is none yet, one with slots of \p SlotSize bytes is made.
  static IndexTable &tableAt(std::vector<IndexTable> &Tables, uint32_t Base,
                             unsigned SlotSize) {
    for (IndexTable &Table : Tables)
      if (Table.Base == Base)
        return Table;
    return Tables.emplace_back(Base, SlotSize);
  }
  static const IndexTable &tableAt(const std::vector<IndexTable> &Tables,
                                   uint32_t Base) {
    return *llvm::find_if(
        Tables, [&](const IndexTable &Table) { return Table.Base == Base; });
  }

  /// Where the kept entry at \p InOffset of Info went, as an offset in this
  /// object's contribution to the output's __debug_info.
  uint32_t infoOutOffset(uint64_t InOffset) const;

  const Redirect *findRedirect(uint32_t InOffset) const {
    auto It = llvm::lower_bound(Redirects, InOffset,
                                [](const Redirect &R, uint32_t Offset) {
                                  return R.InOffset < Offset;
                                });
    return It != Redirects.end() && It->InOffset == InOffset ? &*It : nullptr;
  }

  /// The symbol that the address \p Address at \p Offset of Info is in, or
  /// null: for the operands of location expressions, which can be addresses
  /// of common symbols and of what other files define.
  const AddressRange *findOperand(uint64_t Address, uint64_t Offset) const {
    return findRelocated(InfoExterns, Address, Offset);
  }
  /// The same for the address \p Address in the slot at \p Offset of Addr.
  const AddressRange *findSlot(uint64_t Address, uint64_t Offset) const {
    return findRelocated(AddrExterns, Address, Offset);
  }
  const AddressRange *findRelocated(const ExternAddresses &Externs,
                                    uint64_t Address, uint64_t Offset) const {
    auto It = llvm::lower_bound(Externs.Offsets, Offset);
    if (It == Externs.Offsets.end() || *It != Offset)
      return findContaining(Address);
    const AddressRange &Range = Externs.Ranges[It - Externs.Offsets.begin()];
    return Range.Linked == NotFound ? nullptr : &Range;
  }
  /// What the address \p Address in the slot at \p Offset of Addr is in the
  /// binary, or 0 if it is not there.
  uint64_t linkedSlot(uint64_t Address, uint64_t Offset) const;

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
