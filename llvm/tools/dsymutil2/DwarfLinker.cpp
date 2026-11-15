//===- DwarfLinker.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DwarfLinker.h"
#include "DsymWriter.h"
#include "Dwarf.h"
#include "LineTable.h"
#include "StringTable.h"
#include "dsymutil2.h"
#include "llvm/Support/Parallel.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

namespace {
struct UnitHeader {
  /// Of the whole unit, with the length field.
  uint32_t Size;
  uint32_t AbbrevOffset;
  uint8_t Version;
  uint8_t AddressSize;
  uint8_t HeaderSize;
};
} // namespace

/// Reads the header of the unit at \p Offset of the __debug_info section
/// \p Info. Returns null on success and what is wrong otherwise.
static const char *readUnitHeader(StringRef Info, uint32_t Offset,
                                  UnitHeader &Header) {
  const uint8_t *P = Info.bytes_begin() + Offset;
  uint64_t Available = Info.size() - Offset;
  if (Available < 11)
    return "truncated compile unit";
  uint32_t Length = read32le(P);
  if (Length == dwarf::DW_LENGTH_DWARF64)
    return "DWARF64 is not supported";
  if (Length > Available - 4)
    return "compile unit extends past the end of __debug_info";
  Header.Size = Length + 4;
  Header.Version = read16le(P + 4);
  if (Header.Version >= 5)
    return "DWARF 5 is not supported yet";
  if (Header.Version < 2)
    return "unsupported DWARF version";
  Header.AbbrevOffset = read32le(P + 6);
  Header.AddressSize = P[10];
  Header.HeaderSize = 11;
  if (Header.AddressSize != 8 && Header.AddressSize != 4)
    return "unsupported address size";
  if (Header.Size < Header.HeaderSize)
    return "truncated compile unit";
  return nullptr;
}

static uint64_t readAddress(const uint8_t *P, unsigned Size) {
  return Size == 8 ? read64le(P) : read32le(P);
}

/// Whether \p Form is how a unit of version \p Version writes an offset into
/// another section, for attributes that are such offsets.
static bool isSectionOffset(uint16_t Form, unsigned Version) {
  return Form == dwarf::DW_FORM_sec_offset ||
         (Version < 4 && Form == dwarf::DW_FORM_data4);
}

static bool isReference(uint16_t Form) {
  return Form == dwarf::DW_FORM_ref1 || Form == dwarf::DW_FORM_ref2 ||
         Form == dwarf::DW_FORM_ref4 || Form == dwarf::DW_FORM_ref8 ||
         Form == dwarf::DW_FORM_ref_addr;
}

/// Reads a reference attribute. \p Size is the size of its value.
static uint64_t readReference(const uint8_t *P, uint64_t Size) {
  switch (Size) {
  case 1:
    return *P;
  case 2:
    return read16le(P);
  case 4:
    return read32le(P);
  default:
    return read64le(P);
  }
}

//===----------------------------------------------------------------------===//
// Scan: what to keep.
//
// The rules follow dsymutil's. A function is live if it is in the debug map.
// What is kept is the live functions, everything they reference, and so on,
// and the entries enclosing all of those.
//
// Keeping happens in units of whole subtrees. An entry whose ancestors are
// all namespaces or modules (containers) is a node: it is kept or dropped
// together with its children. The containers are kept as needed: just the
// entry itself, around the kept nodes inside it.
//
// There is one exception, which dsymutil does not make. A function that is
// not live can still be referenced, typically as the callee of a call site in
// a live function when its own copy lost against another object file's. If
// only the function's entry is referenced, and nothing inside it, only that
// entry is kept and its children are dropped: without addresses they
// describe nothing.
//===----------------------------------------------------------------------===//

namespace {
struct Node {
  /// The entry with its children is [Begin, End) in __debug_info, the entry
  /// itself [Begin, OwnEnd).
  uint32_t Begin;
  uint32_t End = 0;
  uint32_t OwnEnd = 0;
  /// The enclosing container, or -1.
  int32_t Parent;
  /// The references made from the entry and its children are Refs[FirstRef,
  /// EndRef), those of the entry itself Refs[FirstRef, EndOwnRef).
  uint32_t FirstRef;
  uint32_t EndRef = 0;
  uint32_t EndOwnRef = 0;
  bool IsContainer = false;
  /// Kept no matter what references it.
  bool IsRoot = false;
  /// A function with a low_pc.
  bool HasCode = false;
  bool IsKept = false;
  /// Kept with its children. Always the case for kept nodes other than
  /// functions that are not live.
  bool IsKeptWhole = false;
};

struct ScannedUnit {
  UnitHeader Header;
  uint32_t Offset;
  uint32_t FirstNode;
  uint32_t EndNode = 0;
  /// The end of the unit's own entry, where its children start.
  uint32_t OwnEnd = 0;
  /// The null entry that ends the unit's children, if it has any.
  uint32_t ClosingNull = 0;
  /// The value of the unit's DW_AT_stmt_list, if it has one.
  uint32_t LineTable = 0;
  bool HasLineTable = false;
  /// The value of the unit's DW_AT_low_pc, if it has one.
  uint64_t LowPc = 0;
  bool HasLowPc = false;
  /// The unit's live functions in Scanner::FunctionRanges.
  uint32_t FirstFunctionRange = 0;
  uint32_t EndFunctionRange = 0;
};

class Scanner {
public:
  explicit Scanner(ObjectFile &Object) : Object(Object) {}

  void run();

private:
  const char *scanUnit(ScannedUnit &Unit);
  int32_t nodeAt(uint32_t Offset) const;
  void keep(int32_t Index, bool Whole);
  void keepReferenced();
  void addRanges(ScannedUnit &Unit);
  void collectFromKeptEntries(const UnitInfo &Unit);
  void addRangeList(const UnitInfo &Unit, uint64_t Offset,
                    const AddressRange *Function);
  void addRangeListEntry(const UnitInfo &Unit, uint64_t Begin, uint64_t End);

  ObjectFile &Object;
  AbbreviationTable Abbreviations;
  std::vector<ScannedUnit> Units;
  /// All nodes of all units, in file order.
  std::vector<Node> Nodes;
  /// Offsets in __debug_info that entries refer to.
  std::vector<uint32_t> Refs;
  /// Kept nodes whose references still have to be followed.
  std::vector<uint32_t> Worklist;
  /// The linked address ranges of the live functions, in file order.
  std::vector<std::pair<uint64_t, uint64_t>> FunctionRanges;
};
} // namespace

const char *Scanner::scanUnit(ScannedUnit &Unit) {
  const UnitHeader &Header = Unit.Header;
  if (!Abbreviations.parse(Object.Abbrev, Header.AbbrevOffset))
    return "malformed abbreviation table";

  const uint8_t *Base = Object.Info.bytes_begin();
  const uint8_t *P = Base + Unit.Offset + Header.HeaderSize;
  const uint8_t *End = Base + Unit.Offset + Header.Size;
  // The containers that are open, innermost last, and the node that is open
  // inside them, if any.
  SmallVector<int32_t, 8> OpenContainers;
  int32_t Current = -1;
  // The number of open entries, and what it was when Current was opened.
  unsigned Depth = 0;
  unsigned CurrentDepth = 0;

  while (P < End) {
    uint32_t Offset = P - Base;
    uint64_t Code = readULEB(P);
    if (Code == 0) {
      if (Depth == 0)
        break;
      --Depth;
      if (Current >= 0) {
        if (Depth == CurrentDepth) {
          Nodes[Current].End = P - Base;
          Nodes[Current].EndRef = Refs.size();
          Current = -1;
        }
      } else if (!OpenContainers.empty()) {
        Nodes[OpenContainers.pop_back_val()].End = P - Base;
      }
      if (Depth == 0) {
        Unit.ClosingNull = Offset;
        break;
      }
      continue;
    }
    const Abbreviation *Abbrev = Abbreviations.find(Code);
    if (!Abbrev)
      return "invalid abbreviation code";

    bool IsUnitEntry = Depth == 0;
    bool StartsNode = !IsUnitEntry && Current < 0;
    bool IsContainer = StartsNode && Abbrev->HasChildren &&
                       (Abbrev->Tag == dwarf::DW_TAG_namespace ||
                        Abbrev->Tag == dwarf::DW_TAG_module);
    if (StartsNode) {
      Node &N = Nodes.emplace_back();
      N.Begin = Offset;
      N.Parent = OpenContainers.empty() ? -1 : OpenContainers.back();
      N.FirstRef = Refs.size();
      N.IsContainer = IsContainer;
      if (!IsContainer)
        Current = Nodes.size() - 1;
      // Base types and imported declarations are always kept, as in
      // dsymutil. (Unlike there, not those inside functions: they go with
      // their function.)
      N.IsRoot = Abbrev->Tag == dwarf::DW_TAG_base_type ||
                 Abbrev->Tag == dwarf::DW_TAG_imported_declaration ||
                 Abbrev->Tag == dwarf::DW_TAG_imported_module ||
                 Abbrev->Tag == dwarf::DW_TAG_imported_unit;
    }
    bool StartsCurrent = StartsNode && !IsContainer;
    bool IsFunction = Abbrev->Tag == dwarf::DW_TAG_subprogram ||
                      Abbrev->Tag == dwarf::DW_TAG_label;

    for (AttributeSpec Spec : Abbreviations.attributes(*Abbrev)) {
      uint64_t Size =
          formSize(Spec.Form, P, Header.Version, Header.AddressSize);
      if (Size == UnsupportedForm)
        return "unsupported attribute form";
      if (Size > uint64_t(End - P))
        return "attribute extends past the end of its unit";
      if (isReference(Spec.Form)) {
        // References made by the unit's own entry or by containers are not
        // followed. They do not occur in practice.
        if (Current >= 0 && Spec.Attribute != dwarf::DW_AT_sibling) {
          uint64_t Target = readReference(P, Size);
          if (Spec.Form != dwarf::DW_FORM_ref_addr)
            Target += Unit.Offset;
          if (Target < Object.Info.size())
            Refs.push_back(Target);
        }
      } else if (Spec.Form == dwarf::DW_FORM_addr && IsFunction &&
                 Spec.Attribute == dwarf::DW_AT_low_pc && Current >= 0) {
        if (StartsCurrent)
          Nodes[Current].HasCode = true;
        if (const AddressRange *Range =
                Object.findStart(readAddress(P, Header.AddressSize))) {
          Nodes[Current].IsRoot = true;
          FunctionRanges.emplace_back(
              Range->Linked, Range->Linked + (Range->End - Range->Begin));
        }
      } else if (IsUnitEntry && Spec.Attribute == dwarf::DW_AT_stmt_list &&
                 isSectionOffset(Spec.Form, Header.Version)) {
        Unit.LineTable = read32le(P);
        Unit.HasLineTable = true;
      } else if (IsUnitEntry && Spec.Attribute == dwarf::DW_AT_low_pc &&
                 Spec.Form == dwarf::DW_FORM_addr) {
        Unit.LowPc = readAddress(P, Header.AddressSize);
        Unit.HasLowPc = true;
      }
      P += Size;
    }

    if (IsUnitEntry)
      Unit.OwnEnd = P - Base;
    if (StartsNode) {
      Nodes.back().OwnEnd = P - Base;
      Nodes.back().EndOwnRef = Refs.size();
    }
    if (IsContainer)
      OpenContainers.push_back(Nodes.size() - 1);
    if (Abbrev->HasChildren) {
      if (StartsCurrent)
        CurrentDepth = Depth;
      ++Depth;
    } else if (IsUnitEntry) {
      break;
    } else if (StartsCurrent) {
      Nodes[Current].End = P - Base;
      Nodes[Current].EndRef = Refs.size();
      Current = -1;
    }
  }
  if (Depth != 0)
    return "compile unit ends in the middle of an entry";
  return nullptr;
}

/// The innermost node that contains \p Offset, or -1.
int32_t Scanner::nodeAt(uint32_t Offset) const {
  auto It = llvm::upper_bound(
      Nodes, Offset,
      [](uint32_t Offset, const Node &N) { return Offset < N.Begin; });
  int32_t Index = int32_t(It - Nodes.begin()) - 1;
  while (Index >= 0 && Offset >= Nodes[Index].End)
    Index = Nodes[Index].Parent;
  return Index;
}

/// Keeps node \p Index: with its children if \p Whole, else at least its own
/// entry.
void Scanner::keep(int32_t Index, bool Whole) {
  Node &N = Nodes[Index];
  if (N.IsContainer) {
    // A reference to a namespace, by a using-directive for example, needs
    // the namespace's entry, not its contents.
    N.IsKept = true;
  } else {
    // Only a function that is not live can be kept without its children.
    if (N.IsRoot || !N.HasCode)
      Whole = true;
    if (!N.IsKept || (Whole && !N.IsKeptWhole)) {
      N.IsKept = true;
      N.IsKeptWhole = Whole;
      Worklist.push_back(Index);
    }
  }
  for (int32_t Parent = N.Parent; Parent >= 0 && !Nodes[Parent].IsKept;
       Parent = Nodes[Parent].Parent)
    Nodes[Parent].IsKept = true;
}

void Scanner::keepReferenced() {
  while (!Worklist.empty()) {
    const Node &N = Nodes[Worklist.back()];
    Worklist.pop_back();
    uint32_t EndRef = N.IsKeptWhole ? N.EndRef : N.EndOwnRef;
    for (uint32_t I = N.FirstRef; I != EndRef; ++I) {
      int32_t Target = nodeAt(Refs[I]);
      if (Target >= 0)
        keep(Target, /*Whole=*/Refs[I] != Nodes[Target].Begin);
    }
  }
}

/// Turns the kept nodes of \p Unit into the list of byte ranges to copy.
void Scanner::addRanges(ScannedUnit &Unit) {
  bool HasKeptNode = false;
  for (uint32_t I = Unit.FirstNode; I != Unit.EndNode && !HasKeptNode; ++I)
    HasKeptNode = Nodes[I].IsKept;
  if (!HasKeptNode)
    return;

  UnitInfo &Info = Object.Units.emplace_back();
  Info.InOffset = Unit.Offset;
  Info.AbbrevOffset = Unit.Header.AbbrevOffset;
  Info.Version = Unit.Header.Version;
  Info.AddressSize = Unit.Header.AddressSize;
  Info.HeaderSize = Unit.Header.HeaderSize;
  Info.FirstRange = Object.KeptRanges.size();
  Info.OutOffset = Object.InfoSize;
  Info.InLowPc = Unit.LowPc;
  Info.HasLowPc = Unit.HasLowPc;

  // The address ranges of the unit: those of its functions, merged.
  MutableArrayRef<std::pair<uint64_t, uint64_t>> Functions =
      MutableArrayRef(FunctionRanges)
          .slice(Unit.FirstFunctionRange,
                 Unit.EndFunctionRange - Unit.FirstFunctionRange);
  llvm::sort(Functions);
  Info.FirstAddressRange = Object.UnitRanges.size();
  for (std::pair<uint64_t, uint64_t> Range : Functions) {
    if (Range.first == Range.second)
      continue;
    if (Object.UnitRanges.size() > Info.FirstAddressRange &&
        Object.UnitRanges.back().second >= Range.first)
      Object.UnitRanges.back().second =
          std::max(Object.UnitRanges.back().second, Range.second);
    else
      Object.UnitRanges.push_back(Range);
  }
  Info.NumAddressRanges = Object.UnitRanges.size() - Info.FirstAddressRange;
  if (Info.NumAddressRanges) {
    // A header, padded to a multiple of the size of a pair of addresses;
    // the pairs; a pair of zeros.
    Object.ArangesSize +=
        16 + 2 * Info.AddressSize * (Info.NumAddressRanges + 1);
  }

  uint32_t Out = Info.OutOffset + Info.HeaderSize;
  auto Add = [&](uint32_t Begin, uint32_t End) {
    if (Begin == End)
      return;
    if (Object.KeptRanges.size() > Info.FirstRange &&
        Object.KeptRanges.back().InEnd == Begin)
      Object.KeptRanges.back().InEnd = End;
    else
      Object.KeptRanges.push_back({Begin, End, Out});
    Out += End - Begin;
  };

  // The unit's own entry, the kept nodes, and the null entries that end the
  // children of the unit and of the kept containers.
  Add(Unit.Offset + Unit.Header.HeaderSize, Unit.OwnEnd);
  SmallVector<const Node *, 8> OpenContainers;
  auto CloseContainers = [&](uint32_t Offset) {
    while (!OpenContainers.empty() && OpenContainers.back()->End <= Offset) {
      const Node *Container = OpenContainers.pop_back_val();
      Add(Container->End - 1, Container->End);
    }
  };
  for (uint32_t I = Unit.FirstNode; I != Unit.EndNode; ++I) {
    const Node &N = Nodes[I];
    CloseContainers(N.Begin);
    if (!N.IsKept)
      continue;
    if (N.IsContainer) {
      Add(N.Begin, N.OwnEnd);
      OpenContainers.push_back(&N);
    } else if (N.IsKeptWhole) {
      Add(N.Begin, N.End);
    } else {
      Add(N.Begin, N.OwnEnd);
      if (N.End != N.OwnEnd)
        Add(N.End - 1, N.End);
    }
  }
  CloseContainers(UINT32_MAX);
  if (Unit.ClosingNull)
    Add(Unit.ClosingNull, Unit.ClosingNull + 1);

  Info.NumRanges = Object.KeptRanges.size() - Info.FirstRange;
  Info.OutSize = Out - Info.OutOffset;
  Object.InfoSize = Out;
  Info.FirstRangeList = Object.RangeListOffsets.size();
  collectFromKeptEntries(Info);

  if (Unit.HasLineTable) {
    Info.LineOffset = Object.LineData.size();
    if (const char *Problem =
            linkLineTable(Object, Unit.LineTable, Object.LineData))
      Object.Warning = Problem;
    else
      Info.HasLineTable = true;
  }
}

/// The address that range lists of \p Unit are relative to in the output.
static uint64_t outBaseAddress(const ObjectFile &Object, const UnitInfo &Unit) {
  return Unit.HasLowPc && Unit.NumAddressRanges
             ? Object.UnitRanges[Unit.FirstAddressRange].first
             : 0;
}

void Scanner::addRangeListEntry(const UnitInfo &Unit, uint64_t Begin,
                                uint64_t End) {
  uint64_t Base = outBaseAddress(Object, Unit);
  char Entry[16];
  if (Unit.AddressSize == 8) {
    write64le(Entry, Begin - Base);
    write64le(Entry + 8, End - Base);
  } else {
    write32le(Entry, Begin - Base);
    write32le(Entry + 4, End - Base);
  }
  Object.RangesData.append(Entry, Entry + 2 * Unit.AddressSize);
}

/// Translates the range list at \p Offset of __debug_ranges, which belongs
/// to an entry in \p Function if that is not null, and adds it to the
/// object's range lists.
void Scanner::addRangeList(const UnitInfo &Unit, uint64_t Offset,
                           const AddressRange *Function) {
  Object.RangeListOffsets.push_back(Object.RangesData.size());
  unsigned Size = Unit.AddressSize;
  uint64_t MaxAddress = Size == 8 ? UINT64_MAX : UINT32_MAX;
  uint64_t Base = Unit.InLowPc;
  StringRef Section = Object.Ranges;
  for (; Offset <= Section.size() && Section.size() - Offset >= 2 * Size;
       Offset += 2 * Size) {
    uint64_t Begin = readAddress(Section.bytes_begin() + Offset, Size);
    uint64_t End = readAddress(Section.bytes_begin() + Offset + Size, Size);
    if (Begin == 0 && End == 0)
      break;
    if (Begin == MaxAddress) {
      Base = End;
      continue;
    }
    if (Begin >= End)
      continue;
    Begin += Base;
    End += Base;
    // Ranges of code that is not in the binary are dropped.
    const AddressRange *Range = Function;
    if (!Range || Begin < Range->Begin || Begin >= Range->End)
      Range = Object.findContaining(Begin);
    if (!Range)
      continue;
    uint64_t Linked = Begin - Range->Begin + Range->Linked;
    addRangeListEntry(Unit, Linked, Linked + (End - Begin));
  }
  addRangeListEntry(Unit, outBaseAddress(Object, Unit),
                    outBaseAddress(Object, Unit));
}

/// Gathers what the layout needs to know about the kept entries of \p Unit:
/// the strings they use, and their range lists, which are translated here.
void Scanner::collectFromKeptEntries(const UnitInfo &Unit) {
  // Usually an object has one unit, whose table is still there.
  if (Units.size() != 1)
    Abbreviations.parse(Object.Abbrev, Unit.AbbrevOffset);
  const uint8_t *Base = Object.Info.bytes_begin();
  bool IsUnitEntry = false;
  bool SeenUnitEntry = false;
  const Abbreviation *Entry = nullptr;
  const AddressRange *Function = nullptr;
  for (const KeptRange &Range :
       ArrayRef(Object.KeptRanges).slice(Unit.FirstRange, Unit.NumRanges))
    forEachEntry(
        Base + Range.InBegin, Base + Range.InEnd, Abbreviations, Unit.Version,
        Unit.AddressSize,
        [&](const Abbreviation &Abbrev) {
          Entry = &Abbrev;
          IsUnitEntry = !SeenUnitEntry;
          SeenUnitEntry = true;
          if (Abbrev.Tag == dwarf::DW_TAG_subprogram)
            Function = nullptr;
        },
        [&](AttributeSpec Spec, const uint8_t *Value, uint64_t Size) {
          if (Spec.Form == dwarf::DW_FORM_strp) {
            Object.StringOffsets.push_back(read32le(Value));
          } else if (Spec.Form == dwarf::DW_FORM_addr &&
                     Spec.Attribute == dwarf::DW_AT_low_pc &&
                     Entry->Tag == dwarf::DW_TAG_subprogram) {
            Function =
                Object.findContaining(readAddress(Value, Unit.AddressSize));
          } else if (Spec.Attribute == dwarf::DW_AT_ranges &&
                     isSectionOffset(Spec.Form, Unit.Version)) {
            if (!IsUnitEntry) {
              addRangeList(Unit, read32le(Value), Function);
              return;
            }
            // The unit's own list is that of its functions.
            Object.RangeListOffsets.push_back(Object.RangesData.size());
            for (std::pair<uint64_t, uint64_t> R :
                 ArrayRef(Object.UnitRanges)
                     .slice(Unit.FirstAddressRange, Unit.NumAddressRanges))
              addRangeListEntry(Unit, R.first, R.second);
            addRangeListEntry(Unit, outBaseAddress(Object, Unit),
                              outBaseAddress(Object, Unit));
          }
        });
}

void Scanner::run() {
  if (Object.Info.empty() || Object.Info.size() > UINT32_MAX)
    return;
  for (uint64_t Offset = 0; Offset < Object.Info.size();) {
    ScannedUnit &Unit = Units.emplace_back();
    Unit.Offset = Offset;
    Unit.FirstNode = Nodes.size();
    Unit.FirstFunctionRange = FunctionRanges.size();
    const char *Problem = readUnitHeader(Object.Info, Offset, Unit.Header);
    if (!Problem)
      Problem = scanUnit(Unit);
    if (Problem) {
      // Without all units, references between units cannot be resolved.
      Object.Warning = Problem;
      return;
    }
    Unit.EndNode = Nodes.size();
    Unit.EndFunctionRange = FunctionRanges.size();
    Offset += Unit.Header.Size;
  }
  // Like dsymutil, take nothing from an object file that has no code in the
  // binary.
  if (FunctionRanges.empty())
    return;

  for (uint32_t I = 0; I != Nodes.size(); ++I)
    if (Nodes[I].IsRoot && !Nodes[I].IsContainer)
      keep(I, /*Whole=*/true);
  keepReferenced();
  for (ScannedUnit &Unit : Units)
    addRanges(Unit);
  llvm::sort(Object.StringOffsets);
  Object.StringOffsets.erase(llvm::unique(Object.StringOffsets),
                             Object.StringOffsets.end());
}

void DwarfLinker::scan() {
  parallelFor(0, Objects.size(), [&](size_t I) { Scanner(Objects[I]).run(); });
}

//===----------------------------------------------------------------------===//
// Layout.
//===----------------------------------------------------------------------===//

void DwarfLinker::mergeStrings() {
  StrSize = dsymutil2::mergeStrings(Objects);
}

void DwarfLinker::layout(DsymWriter &Writer) {
  for (ObjectFile &Object : Objects) {
    if (Object.Units.empty())
      continue;
    Object.InfoOffset = InfoSize;
    InfoSize += Object.InfoSize;
    // The abbreviation tables are copied whole.
    Object.AbbrevOffset = AbbrevSize;
    AbbrevSize += Object.Abbrev.size();
    Object.LineOffset = LineSize;
    LineSize += Object.LineData.size();
    Object.RangesOffset = RangesSize;
    RangesSize += Object.RangesData.size();
    Object.ArangesOffset = ArangesSize;
    ArangesSize += Object.ArangesSize;
  }
  // Offsets into these sections are 32 bits wide in DWARF32.
  if (InfoSize > UINT32_MAX || AbbrevSize > UINT32_MAX ||
      LineSize > UINT32_MAX || RangesSize > UINT32_MAX)
    fatal("the debug info does not fit in 4 GB sections");
  if (InfoSize == 0)
    return;
  InfoSection = Writer.addSection("__debug_info", InfoSize);
  AbbrevSection = Writer.addSection("__debug_abbrev", AbbrevSize);
  if (LineSize)
    LineSection = Writer.addSection("__debug_line", LineSize);
  if (RangesSize)
    RangesSection = Writer.addSection("__debug_ranges", RangesSize);
  if (ArangesSize)
    ArangesSection = Writer.addSection("__debug_aranges", ArangesSize);
  if (StrSize)
    StrSection = Writer.addSection("__debug_str", StrSize);
}

//===----------------------------------------------------------------------===//
// Write: copy and patch.
//===----------------------------------------------------------------------===//

namespace {
/// Copies the kept ranges of one object's units to the output and patches
/// them there.
class UnitWriter {
public:
  /// \p Out is where the object's contribution to __debug_info goes.
  UnitWriter(const ObjectFile &Object, uint8_t *Out)
      : Object(Object), Out(Out) {}

  void write(const UnitInfo &Unit);
  /// Writes the unit's entry in __debug_aranges to \p Out and returns its
  /// size.
  uint64_t writeAranges(const UnitInfo &Unit, uint8_t *Out) const;

private:
  void patchRange(const UnitInfo &Unit, uint8_t *P, uint8_t *End);
  uint32_t outOffset(uint64_t InOffset) const;

  const ObjectFile &Object;
  uint8_t *Out;
  AbbreviationTable Abbreviations;
  /// The live function that the entries being patched are in, if any.
  const AddressRange *Function = nullptr;
  /// Whether the next entry is the unit's own.
  bool AtUnitEntry = false;
  /// The next of the unit's range lists in ObjectFile::RangeListOffsets.
  uint32_t NextRangeList = 0;
};
} // namespace

/// Where the entry at \p InOffset of the object's __debug_info went, as an
/// offset in the object's contribution to the output.
uint32_t UnitWriter::outOffset(uint64_t InOffset) const {
  auto It = llvm::upper_bound(Object.KeptRanges, InOffset,
                              [](uint64_t Offset, const KeptRange &Range) {
                                return Offset < Range.InBegin;
                              });
  if (It == Object.KeptRanges.begin())
    return 0;
  const KeptRange &Range = *(It - 1);
  if (InOffset < Range.InEnd)
    return Range.Out + (InOffset - Range.InBegin);
  // The target was dropped, which the scan rules out for everything but
  // DW_AT_sibling. Point at what follows.
  return It != Object.KeptRanges.end() ? It->Out : Object.InfoSize;
}

void UnitWriter::write(const UnitInfo &Unit) {
  uint8_t *Header = Out + Unit.OutOffset;
  memcpy(Header, Object.Info.data() + Unit.InOffset, Unit.HeaderSize);
  write32le(Header, Unit.OutSize - 4);
  write32le(Header + 6, Object.AbbrevOffset + Unit.AbbrevOffset);

  // The scan has parsed this table before, successfully.
  Abbreviations.parse(Object.Abbrev, Unit.AbbrevOffset);
  AtUnitEntry = true;
  NextRangeList = Unit.FirstRangeList;
  for (const KeptRange &Range :
       ArrayRef(Object.KeptRanges).slice(Unit.FirstRange, Unit.NumRanges)) {
    uint32_t Size = Range.InEnd - Range.InBegin;
    memcpy(Out + Range.Out, Object.Info.data() + Range.InBegin, Size);
    patchRange(Unit, Out + Range.Out, Out + Range.Out + Size);
  }
}

/// Patches the entries in [P, End), which is a copy of a kept range.
void UnitWriter::patchRange(const UnitInfo &Unit, uint8_t *P, uint8_t *End) {
  const Abbreviation *Entry = nullptr;
  // Set when the entry's low_pc is not in the binary: the entry describes
  // code that did not survive the link.
  bool IsDead = false;
  bool IsUnitEntry = false;
  // The unit covers the addresses from its first function to its last.
  ArrayRef<std::pair<uint64_t, uint64_t>> UnitRanges =
      ArrayRef(Object.UnitRanges)
          .slice(Unit.FirstAddressRange, Unit.NumAddressRanges);
  uint64_t UnitLowPc = UnitRanges.empty() ? 0 : UnitRanges.front().first;
  uint64_t UnitHighPc = UnitRanges.empty() ? 0 : UnitRanges.back().second;
  auto WriteAddress = [&](uint8_t *P, uint64_t Address) {
    if (Unit.AddressSize == 8)
      write64le(P, Address);
    else
      write32le(P, Address);
  };
  forEachEntry(
      P, End, Abbreviations, Unit.Version, Unit.AddressSize,
      [&](const Abbreviation &Abbrev) {
        Entry = &Abbrev;
        IsDead = false;
        IsUnitEntry = AtUnitEntry;
        AtUnitEntry = false;
        if (Abbrev.Tag == dwarf::DW_TAG_subprogram)
          Function = nullptr;
      },
      [&](AttributeSpec Spec, const uint8_t *Value, uint64_t Size) {
        // The bytes are this function's to change.
        uint8_t *P = const_cast<uint8_t *>(Value);
        if (IsUnitEntry && Spec.Attribute == dwarf::DW_AT_low_pc &&
            Spec.Form == dwarf::DW_FORM_addr) {
          WriteAddress(P, UnitLowPc);
          return;
        }
        if (IsUnitEntry && Spec.Attribute == dwarf::DW_AT_high_pc) {
          // An address, or a length.
          if (Spec.Form == dwarf::DW_FORM_addr)
            WriteAddress(P, UnitHighPc);
          else if (Spec.Form == dwarf::DW_FORM_data4)
            write32le(P, UnitHighPc - UnitLowPc);
          else if (Spec.Form == dwarf::DW_FORM_data8)
            write64le(P, UnitHighPc - UnitLowPc);
          return;
        }
        switch (Spec.Form) {
        case dwarf::DW_FORM_addr: {
          uint64_t Address = readAddress(P, Unit.AddressSize);
          const AddressRange *Range = nullptr;
          if (Spec.Attribute == dwarf::DW_AT_low_pc) {
            Range = Object.findContaining(Address);
            if (!Range)
              IsDead = true;
            else if (Entry->Tag == dwarf::DW_TAG_subprogram)
              Function = Range;
          } else if (!IsDead) {
            // An address in a function can be the function's end, which is
            // where the next function starts: try the function first.
            if (Function && Address >= Function->Begin &&
                Address <= Function->End)
              Range = Function;
            else
              Range = Object.findContaining(Address);
          }
          WriteAddress(P,
                       Range ? Address - Range->Begin + Range->Linked : 0);
          break;
        }
        case dwarf::DW_FORM_data1:
        case dwarf::DW_FORM_data2:
        case dwarf::DW_FORM_data4:
        case dwarf::DW_FORM_data8:
          // The length of code that is not there.
          if (IsDead && Spec.Attribute == dwarf::DW_AT_high_pc)
            memset(P, 0, Size);
          [[fallthrough]];
        case dwarf::DW_FORM_sec_offset:
          if (!isSectionOffset(Spec.Form, Unit.Version))
            break;
          if (Spec.Attribute == dwarf::DW_AT_stmt_list && Unit.HasLineTable)
            write32le(P, Object.LineOffset + Unit.LineOffset);
          else if (Spec.Attribute == dwarf::DW_AT_ranges)
            // The scan translated the lists in the order of their entries.
            write32le(P, Object.RangesOffset +
                             Object.RangeListOffsets[NextRangeList++]);
          break;
        case dwarf::DW_FORM_strp:
          write32le(P, Object.stringOutOffset(read32le(P)));
          break;
        case dwarf::DW_FORM_ref1:
        case dwarf::DW_FORM_ref2:
        case dwarf::DW_FORM_ref4:
        case dwarf::DW_FORM_ref8: {
          // Relative to the unit. Entries only move towards the start of
          // their unit, so the new value fits.
          uint64_t Target = outOffset(Unit.InOffset + readReference(P, Size)) -
                            Unit.OutOffset;
          if (Size == 1)
            *P = Target;
          else if (Size == 2)
            write16le(P, Target);
          else if (Size == 4)
            write32le(P, Target);
          else
            write64le(P, Target);
          break;
        }
        case dwarf::DW_FORM_ref_addr: {
          // Relative to the section, and to an entry of this object file.
          uint64_t Target =
              Object.InfoOffset + outOffset(readReference(P, Size));
          if (Size == 4)
            write32le(P, Target);
          else
            write64le(P, Target);
          break;
        }
        default:
          break;
        }
      });
}

uint64_t UnitWriter::writeAranges(const UnitInfo &Unit, uint8_t *Out) const {
  if (Unit.NumAddressRanges == 0)
    return 0;
  unsigned AddressSize = Unit.AddressSize;
  uint64_t Size = 16 + 2 * AddressSize * (Unit.NumAddressRanges + 1);
  memset(Out, 0, Size);
  write32le(Out, Size - 4);
  write16le(Out + 4, 2); // version
  write32le(Out + 6, Object.InfoOffset + Unit.OutOffset);
  Out[10] = AddressSize;
  Out[11] = 0; // segment selector size
  uint8_t *P = Out + 16;
  for (std::pair<uint64_t, uint64_t> Range :
       ArrayRef(Object.UnitRanges)
           .slice(Unit.FirstAddressRange, Unit.NumAddressRanges)) {
    if (AddressSize == 8) {
      write64le(P, Range.first);
      write64le(P + 8, Range.second - Range.first);
    } else {
      write32le(P, Range.first);
      write32le(P + 4, Range.second - Range.first);
    }
    P += 2 * AddressSize;
  }
  return Size;
}

void DwarfLinker::write(DsymWriter &Writer) {
  if (InfoSize == 0)
    return;
  char *Info = Writer.sectionData(InfoSection);
  char *Abbrev = Writer.sectionData(AbbrevSection);
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    if (Object.Units.empty())
      return;
    UnitWriter Units(Object,
                     reinterpret_cast<uint8_t *>(Info) + Object.InfoOffset);
    uint8_t *Aranges =
        ArangesSize ? reinterpret_cast<uint8_t *>(
                          Writer.sectionData(ArangesSection)) +
                          Object.ArangesOffset
                    : nullptr;
    for (const UnitInfo &Unit : Object.Units) {
      Units.write(Unit);
      Aranges += Units.writeAranges(Unit, Aranges);
    }
    if (!Object.RangesData.empty())
      memcpy(Writer.sectionData(RangesSection) + Object.RangesOffset,
             Object.RangesData.data(), Object.RangesData.size());
    memcpy(Abbrev + Object.AbbrevOffset, Object.Abbrev.data(),
           Object.Abbrev.size());
    if (!Object.LineData.empty())
      memcpy(Writer.sectionData(LineSection) + Object.LineOffset,
             Object.LineData.data(), Object.LineData.size());
  });
  if (StrSize)
    writeStrings(Objects, Writer.sectionData(StrSection));
}
