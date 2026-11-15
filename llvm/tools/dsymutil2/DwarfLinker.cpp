//===- DwarfLinker.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DwarfLinker.h"
#include "AccelTable.h"
#include "DsymWriter.h"
#include "Dwarf.h"
#include "FrameTable.h"
#include "LineTable.h"
#include "StringTable.h"
#include "dsymutil2.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/bit.h"
#include "llvm/DebugInfo/DWARF/DWARFAcceleratorTable.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/xxhash.h"
#include <atomic>

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

/// Reads a constant attribute whose value has \p Size bytes.
static uint64_t readUnsigned(uint16_t Form, const uint8_t *P, uint64_t Size) {
  if (Form == dwarf::DW_FORM_udata)
    return readULEB(P);
  return Size <= 8 ? readReference(P, Size) : 0;
}

/// Writes a value of \p Size bytes.
static void writeUnsigned(uint8_t *P, uint64_t Size, uint64_t Value) {
  switch (Size) {
  case 1:
    *P = Value;
    break;
  case 2:
    write16le(P, Value);
    break;
  case 4:
    write32le(P, Value);
    break;
  default:
    write64le(P, Value);
    break;
  }
}

/// Whether attribute \p Spec of a unit of version \p Version is the offset
/// of a location list in __debug_loc.
static bool isLocationList(AttributeSpec Spec, unsigned Version) {
  switch (Spec.Attribute) {
  case dwarf::DW_AT_data_member_location:
    // A DW_FORM_data4 is the offset of the member here, also in the old
    // versions where it could be either.
    return Spec.Form == dwarf::DW_FORM_sec_offset;
  case dwarf::DW_AT_location:
  case dwarf::DW_AT_string_length:
  case dwarf::DW_AT_return_addr:
  case dwarf::DW_AT_frame_base:
  case dwarf::DW_AT_static_link:
  case dwarf::DW_AT_segment:
  case dwarf::DW_AT_use_location:
  case dwarf::DW_AT_vtable_elem_location:
    return isSectionOffset(Spec.Form, Version);
  default:
    return false;
  }
}

/// Not an offset in __debug_info.
constexpr uint64_t NoOffset = UINT64_MAX;

/// Turns the addresses in the location expression \p Expression from those
/// of the object file into those of the binary. \p InfoOffset is where the
/// expression is in the object's __debug_info, or NoOffset.
///
/// An address that is not in the binary becomes 0, as in dsymutil. If
/// \p IsVariable, the expression is the location of a variable, which then
/// has none: dsymutil removes the attribute, and here the expression is
/// overwritten with operations that do nothing.
static void patchExpression(const ObjectFile &Object,
                            MutableArrayRef<uint8_t> Expression,
                            unsigned AddressSize, uint64_t InfoOffset,
                            bool IsVariable) {
  bool IsDead = false;
  forEachAddressOperand(
      Expression, AddressSize, [&](size_t Offset, unsigned Size) {
        uint8_t *P = Expression.data() + Offset;
        uint64_t Address = readReference(P, Size);
        const AddressRange *Range = Object.findOperand(
            Address, InfoOffset == NoOffset ? NoOffset : InfoOffset + Offset);
        IsDead |= !Range;
        writeUnsigned(P, Size,
                      Range ? Address - Range->Begin + Range->Linked : 0);
      });
  if (IsDead && IsVariable)
    memset(Expression.data(), dwarf::DW_OP_nop, Expression.size());
}

/// Whether the flag attribute at \p P is set.
static bool readFlag(uint16_t Form, const uint8_t *P) {
  return Form == dwarf::DW_FORM_flag_present ||
         (Form == dwarf::DW_FORM_flag && *P);
}

/// The tags of the entries that go into the accelerator table of types: the
/// same as in dsymutil.
static bool isTypeTag(uint16_t Tag) {
  switch (Tag) {
  case dwarf::DW_TAG_array_type:
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_enumeration_type:
  case dwarf::DW_TAG_pointer_type:
  case dwarf::DW_TAG_reference_type:
  case dwarf::DW_TAG_string_type:
  case dwarf::DW_TAG_structure_type:
  case dwarf::DW_TAG_subroutine_type:
  case dwarf::DW_TAG_template_alias:
  case dwarf::DW_TAG_typedef:
  case dwarf::DW_TAG_union_type:
  case dwarf::DW_TAG_ptr_to_member_type:
  case dwarf::DW_TAG_set_type:
  case dwarf::DW_TAG_subrange_type:
  case dwarf::DW_TAG_base_type:
  case dwarf::DW_TAG_const_type:
  case dwarf::DW_TAG_constant:
  case dwarf::DW_TAG_file_type:
  case dwarf::DW_TAG_namelist:
  case dwarf::DW_TAG_packed_type:
  case dwarf::DW_TAG_volatile_type:
  case dwarf::DW_TAG_restrict_type:
  case dwarf::DW_TAG_atomic_type:
  case dwarf::DW_TAG_interface_type:
  case dwarf::DW_TAG_unspecified_type:
  case dwarf::DW_TAG_shared_type:
  case dwarf::DW_TAG_immutable_type:
    return true;
  default:
    return false;
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
//
// Uniquing. Some entries say the same thing in every object file: a clang
// module is described wherever it is imported, and a C++ type wherever it
// is used, which the language requires to be the same type everywhere (the
// one definition rule, ODR). Such an entry gets an identity, a hash of what
// it and its parents say. Of all the kept entries with one identity, the one
// in the first object file, in debug map order, is the owner. An object
// keeps the entries it owns, and its references to entries that another
// object owns go to the owner's copy instead of keeping its own.
//
// The entries that are shared like this are modules, and classes, structs,
// unions, enums and typedefs with a name, outside of functions and of other
// types, in units of languages that have the rule. The namespaces that a
// type is in are part of its identity.
//
// So that the result does not depend on the order in which threads get to
// the object files, this takes three passes: scan() finds every object's
// nodes and what it would keep without uniquing, recordOwners() finds the
// owners among those, and resolve() decides what is kept in the end.
//===----------------------------------------------------------------------===//

namespace {
/// The identity that nodes without an enclosing node have "inside" them.
constexpr uint64_t RootIdentity = 0x9e3779b97f4a7c15;

uint64_t mixIdentity(uint64_t A, uint64_t B) {
  uint64_t Hash = (A ^ B) * 0x9e3779b97f4a7c15;
  return Hash ^ (Hash >> 32);
}

/// Whether an entry with tag \p Tag, outside of functions and types, gets an
/// identity. \p HasODR says whether its unit is in a language with the one
/// definition rule.
bool hasIdentity(uint16_t Tag, bool HasODR) {
  switch (Tag) {
  case dwarf::DW_TAG_module:
    return true;
  case dwarf::DW_TAG_namespace:
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_structure_type:
  case dwarf::DW_TAG_union_type:
  case dwarf::DW_TAG_enumeration_type:
  case dwarf::DW_TAG_typedef:
    return HasODR;
  default:
    return false;
  }
}

/// For every identity, its owner: the first of the object files that keep a
/// node with it, and the node's offset there. Filled concurrently.
class OwnerTable {
public:
  explicit OwnerTable(size_t NumNodes)
      : Keys(llvm::bit_ceil(std::max<size_t>(2 * NumNodes, 16))),
        Owners(Keys.size()) {}

  static uint64_t owner(uint32_t Object, uint32_t Offset) {
    return (uint64_t(Object) + 1) << 32 | Offset;
  }
  static uint32_t objectOf(uint64_t Owner) { return (Owner >> 32) - 1; }
  static uint32_t offsetOf(uint64_t Owner) { return Owner; }

  /// Says that object \p Object keeps a node with identity \p Key at
  /// \p Offset.
  void record(uint64_t Key, uint32_t Object, uint32_t Offset) {
    std::atomic<uint64_t> &Cell = Owners[find(Key, /*Insert=*/true)];
    uint64_t New = owner(Object, Offset);
    uint64_t Old = Cell.load(std::memory_order_relaxed);
    while ((Old == 0 || New < Old) && !Cell.compare_exchange_weak(Old, New))
      ;
  }

  /// The owner of \p Key, or 0 if nothing was recorded for it.
  uint64_t lookup(uint64_t Key) const {
    size_t I = const_cast<OwnerTable *>(this)->find(Key, /*Insert=*/false);
    return I == NotFound ? 0 : Owners[I].load(std::memory_order_relaxed);
  }

private:
  static constexpr size_t NotFound = ~size_t(0);

  size_t find(uint64_t Key, bool Insert) {
    size_t Mask = Keys.size() - 1;
    for (size_t I = (Key >> 1) & Mask;; I = (I + 1) & Mask) {
      uint64_t Current = Keys[I].load(std::memory_order_relaxed);
      if (Current == Key)
        return I;
      if (Current != 0)
        continue;
      if (!Insert)
        return NotFound;
      if (Keys[I].compare_exchange_strong(Current, Key) || Current == Key)
        return I;
    }
  }

  /// Open addressing; an identity is never 0.
  std::vector<std::atomic<uint64_t>> Keys;
  std::vector<std::atomic<uint64_t>> Owners;
};

/// Set in an element of Scanner::Refs if the reference can be redirected to
/// another object's entry: it then has to become a DW_FORM_ref_addr of four
/// bytes. That leaves 31 bits for the offset.
constexpr uint32_t RedirectableRef = uint32_t(1) << 31;
/// An element of Scanner::Refs is at first the offset that an entry refers
/// to. When the reference is followed for the first time, it becomes the
/// index of the node that the offset is in, or NoNode, with this bit set if
/// the offset is that of the node's own entry: marking what is kept happens
/// twice, and finding the node of an offset is most of its work.
constexpr uint32_t RefToOwnEntry = uint32_t(1) << 30;
constexpr uint32_t NoNode = RefToOwnEntry - 1;

struct Node {
  /// The entry with its children is [Begin, End) in __debug_info, the entry
  /// itself [Begin, OwnEnd).
  uint32_t Begin;
  uint32_t End = 0;
  uint32_t OwnEnd = 0;
  /// The enclosing container, or -1.
  int32_t Parent;
  /// The references made from the entry and its children are Refs[FirstRef,
  /// EndRef), those of the entry itself Refs[FirstRef, EndOwnRef). Those in
  /// Refs[FirstRef, EndNodeRef) have been turned into node indexes.
  uint32_t FirstRef;
  uint32_t EndRef = 0;
  uint32_t EndOwnRef = 0;
  uint32_t EndNodeRef = 0;
  bool IsContainer = false;
  /// Kept no matter what references it.
  bool IsRoot = false;
  /// A function with a low_pc.
  bool HasCode = false;
  bool IsKept = false;
  /// Kept with its children. Always the case for kept nodes other than
  /// functions that are not live.
  bool IsKeptWhole = false;
  /// What makes this entry the same as entries of other object files, or 0.
  uint64_t Identity = 0;
  /// Whether one copy of the entry is kept for all object files that have
  /// it. Not the case for a namespace: every unit keeps an entry for it
  /// around what it keeps of the namespace, as with dsymutil. Its identity
  /// is only a part of those of the entries in it.
  bool IsShared = false;
};

constexpr uint32_t NoString = UINT32_MAX;

/// What an entry says about its names.
struct EntryNames {
  /// Offsets in the object's strings, or NoString.
  uint32_t Name = NoString;
  uint32_t LinkageName = NoString;
  /// The entry that this one specifies or is an instance of, which has the
  /// names that this one lacks: its offset in __debug_info, or 0.
  uint32_t Origin = 0;
};

/// An entry that the entry at hand is in.
struct Scope {
  /// The hash of its name with the names of the scopes it is in, as far as
  /// they count.
  uint32_t QualifiedNameHash;
  /// A unit or a module: what is in it is not named after it.
  bool EndsQualifiedNames;
  /// The function that the scope is or is in, if that is in the binary.
  const AddressRange *Function;
  /// Whether the scope is a function or in one.
  bool InFunction;
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
  /// Whether the unit's language has the one definition rule: a type with a
  /// name is the same type in all units.
  bool HasODR = false;
  /// The value of the unit's DW_AT_low_pc, if it has one.
  uint64_t LowPc = 0;
  bool HasLowPc = false;
  /// The unit's live functions in Scanner::FunctionRanges.
  uint32_t FirstFunctionRange = 0;
  uint32_t EndFunctionRange = 0;
};

class Scanner {
public:
  Scanner(ObjectFile &Object, uint32_t Index) : Object(Object), Index(Index) {}

  /// Finds the nodes of the object file and what is kept without uniquing.
  void scan();
  /// The number of kept nodes that have an identity.
  size_t numIdentities() const { return NumIdentities; }
  void recordOwners(OwnerTable &Owners) const;
  /// Decides what is kept, and prepares the object file's contributions to
  /// the output as far as they do not depend on other object files.
  void resolve(const OwnerTable &Owners);

private:
  const char *scanUnit(ScannedUnit &Unit);
  uint64_t stringHash(uint32_t Offset);
  int32_t nodeAt(uint32_t Offset) const;
  void markKept(const OwnerTable *Owners);
  void keep(int32_t Index, bool Whole);
  void keepReferenced(const OwnerTable *Owners);
  void addRanges(ScannedUnit &Unit);
  void collectFromKeptEntries(const UnitInfo &Unit);
  bool addRangeList(const UnitInfo &Unit, uint64_t Offset,
                    const AddressRange *Function);
  void addLocationList(const UnitInfo &Unit, uint64_t Offset,
                       const AddressRange *Function);
  void addRangeListEntry(const UnitInfo &Unit, uint64_t Begin, uint64_t End);
  uint32_t addString(StringRef Str);
  void noteName(EntryNames &Names, AttributeSpec Spec, const uint8_t *Value,
                uint64_t Size, uint32_t UnitOffset);
  EntryNames readNames(uint32_t Offset);
  void completeNames(EntryNames &Names);
  void useExtraString(uint32_t Name);
  void addName(std::vector<AccelName> &Table, uint32_t Name, uint32_t Entry);
  void addCodeNames(EntryNames Names, uint16_t Tag, uint32_t Entry);

  ObjectFile &Object;
  /// The index of Object in the debug map.
  uint32_t Index;
  /// The table of the unit that is being worked on, and where it is in
  /// __debug_abbrev.
  AbbreviationTable Abbreviations;
  uint32_t AbbrevOffset = 0;
  /// The table of the unit that readNames() last read an entry of, if that
  /// was another unit.
  AbbreviationTable OriginAbbreviations;
  uint32_t OriginAbbrevOffset = UINT32_MAX;
  /// The names that completeNames() found for an entry that others are
  /// instances of, by its offset: name and linkage name.
  DenseMap<uint32_t, std::pair<uint32_t, uint32_t>> OriginNames;
  /// The name that namespaces without a name are found by.
  uint32_t AnonymousNamespace = NoString;
  std::vector<ScannedUnit> Units;
  /// All nodes of all units, in file order.
  std::vector<Node> Nodes;
  /// What entries refer to: offsets in __debug_info or, later, nodes, with
  /// RedirectableRef.
  std::vector<uint32_t> Refs;
  /// Hashes of strings of __debug_str, by offset.
  DenseMap<uint32_t, uint64_t> StringHashes;
  size_t NumIdentities = 0;
  /// Kept nodes whose references still have to be followed.
  std::vector<uint32_t> Worklist;
  /// The linked address ranges of the live functions, in file order.
  std::vector<std::pair<uint64_t, uint64_t>> FunctionRanges;
  bool HasLiveVariable = false;
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
    // A variable outside of functions and types.
    bool IsVariable = StartsCurrent && (Abbrev->Tag == dwarf::DW_TAG_variable ||
                                        Abbrev->Tag == dwarf::DW_TAG_constant);
    // Two entries are the same if their parents are the same and they have
    // the same tag and attributes, where references to other entries, which
    // are offsets, and the number of the file in the unit's line table do
    // not count. An entry without a name is nobody else's, except for a
    // module.
    uint64_t Identity = 0;
    bool NeedsName = Abbrev->Tag != dwarf::DW_TAG_module;
    if (StartsNode && hasIdentity(Abbrev->Tag, Unit.HasODR)) {
      int32_t Parent = Nodes.back().Parent;
      Identity = Parent < 0 ? RootIdentity : Nodes[Parent].Identity;
      if (Identity)
        Identity = mixIdentity(Identity, Abbrev->Tag) | 1;
    }
    // The symbol of the function that this entry describes, if it is live.
    AddressRange *LiveFunction = nullptr;

    for (AttributeSpec Spec : Abbreviations.attributes(*Abbrev)) {
      uint64_t Size =
          formSize(Spec.Form, P, Header.Version, Header.AddressSize);
      if (Size == UnsupportedForm)
        return "unsupported attribute form";
      if (Size > uint64_t(End - P))
        return "attribute extends past the end of its unit";
      if (Identity && !isReference(Spec.Form) &&
          Spec.Attribute != dwarf::DW_AT_decl_file) {
        uint64_t Value = Spec.Form == dwarf::DW_FORM_strp
                             ? stringHash(read32le(P))
                             : xxh3_64bits(ArrayRef(P, Size));
        Identity = mixIdentity(
            mixIdentity(Identity, uint64_t(Spec.Attribute) << 16 | Spec.Form),
            Value);
        // 0 means that there is none.
        Identity |= 1;
        NeedsName &= Spec.Attribute != dwarf::DW_AT_name;
      }
      if (isReference(Spec.Form)) {
        // References made by the unit's own entry or by containers are not
        // followed. They do not occur in practice.
        if (Current >= 0 && Spec.Attribute != dwarf::DW_AT_sibling) {
          uint64_t Target = readReference(P, Size);
          if (Spec.Form != dwarf::DW_FORM_ref_addr)
            Target += Unit.Offset;
          bool IsRedirectable = Size == 4 &&
                                (Spec.Form == dwarf::DW_FORM_ref4 ||
                                 Spec.Form == dwarf::DW_FORM_ref_addr);
          if (Target < Object.Info.size())
            Refs.push_back(Target | (IsRedirectable ? RedirectableRef : 0));
        }
      } else {
        switch (Spec.Attribute) {
        case dwarf::DW_AT_low_pc:
          if (Spec.Form != dwarf::DW_FORM_addr)
            break;
          if (IsUnitEntry) {
            Unit.LowPc = readAddress(P, Header.AddressSize);
            Unit.HasLowPc = true;
          } else if (IsFunction && Current >= 0) {
            if (StartsCurrent)
              Nodes[Current].HasCode = true;
            LiveFunction = Object.findStart(readAddress(P, Header.AddressSize));
            if (LiveFunction)
              Nodes[Current].IsRoot = true;
          }
          break;
        case dwarf::DW_AT_high_pc: {
          if (!LiveFunction)
            break;
          // An address, or a length. The symbol can be longer than the
          // function, by the padding that follows it; the debug info should
          // not cover that.
          uint64_t FunctionEnd =
              Spec.Form == dwarf::DW_FORM_addr
                  ? readAddress(P, Header.AddressSize)
                  : LiveFunction->Begin + readUnsigned(Spec.Form, P, Size);
          if (FunctionEnd > LiveFunction->Begin &&
              FunctionEnd < LiveFunction->End)
            LiveFunction->End = FunctionEnd;
          break;
        }
        case dwarf::DW_AT_const_value:
          // Constants are always kept, as in dsymutil.
          if (IsVariable)
            Nodes[Current].IsRoot = true;
          break;
        case dwarf::DW_AT_location: {
          if (!IsVariable || !isBlock(Spec.Form))
            break;
          // A variable is live if it is where a symbol of the debug map is.
          ArrayRef<uint8_t> Location = blockContents(Spec.Form, P);
          forEachAddressOperand(
              Location, Header.AddressSize, [&](size_t Offset, unsigned Size) {
                if (Object.findOperand(
                        readReference(Location.data() + Offset, Size),
                        Location.data() + Offset - Base))
                  Nodes[Current].IsRoot = HasLiveVariable = true;
              });
          break;
        }
        case dwarf::DW_AT_stmt_list:
          if (IsUnitEntry && isSectionOffset(Spec.Form, Header.Version)) {
            Unit.LineTable = read32le(P);
            Unit.HasLineTable = true;
          }
          break;
        case dwarf::DW_AT_language:
          if (IsUnitEntry) {
            auto Language =
                dwarf::SourceLanguage(readUnsigned(Spec.Form, P, Size));
            Unit.HasODR = dwarf::isCPlusPlus(Language) ||
                          Language == dwarf::DW_LANG_ObjC_plus_plus;
          }
          break;
        default:
          break;
        }
      }
      P += Size;
    }

    if (LiveFunction)
      FunctionRanges.emplace_back(LiveFunction->Linked,
                                  LiveFunction->Linked + (LiveFunction->End -
                                                          LiveFunction->Begin));
    if (IsUnitEntry)
      Unit.OwnEnd = P - Base;
    if (StartsNode) {
      Nodes.back().OwnEnd = P - Base;
      Nodes.back().EndOwnRef = Refs.size();
      Nodes.back().Identity = NeedsName ? 0 : Identity;
      Nodes.back().IsShared =
          Nodes.back().Identity && Abbrev->Tag != dwarf::DW_TAG_namespace;
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

uint64_t Scanner::stringHash(uint32_t Offset) {
  auto [It, Inserted] = StringHashes.try_emplace(Offset);
  if (Inserted) {
    StringRef Str = Object.Str.substr(Offset);
    It->second = xxh3_64bits(Str.substr(0, Str.find('\0')));
  }
  return It->second;
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

/// Keeps what the nodes on the worklist refer to, and so on. With \p Owners,
/// a reference to a node that another node owns is redirected there instead.
void Scanner::keepReferenced(const OwnerTable *Owners) {
  while (!Worklist.empty()) {
    Node &N = Nodes[Worklist.back()];
    Worklist.pop_back();
    uint32_t EndRef = N.IsKeptWhole ? N.EndRef : N.EndOwnRef;
    for (uint32_t I = std::max(N.FirstRef, N.EndNodeRef); I < EndRef; ++I) {
      uint32_t Offset = Refs[I] & ~RedirectableRef;
      int32_t Target = nodeAt(Offset);
      Refs[I] =
          (Refs[I] & RedirectableRef) |
          (Target < 0
               ? NoNode
               : Target | (Offset == Nodes[Target].Begin ? RefToOwnEntry : 0));
    }
    N.EndNodeRef = std::max(N.EndNodeRef, EndRef);
    for (uint32_t I = N.FirstRef; I != EndRef; ++I) {
      uint32_t Target = Refs[I] & NoNode;
      if (Target == NoNode)
        continue;
      const Node &T = Nodes[Target];
      // Only references to the node's own entry can be redirected: what is
      // where inside the owner's copy is not known.
      bool IsToOwnEntry = Refs[I] & RefToOwnEntry;
      if (Owners && T.IsShared && (Refs[I] & RedirectableRef) && IsToOwnEntry) {
        uint64_t Owner = Owners->lookup(T.Identity);
        if (Owner && Owner != OwnerTable::owner(Index, T.Begin)) {
          Object.Redirects.push_back({T.Begin, OwnerTable::objectOf(Owner),
                                      OwnerTable::offsetOf(Owner)});
          continue;
        }
      }
      keep(Target, /*Whole=*/!IsToOwnEntry);
    }
  }
}

/// Decides what is kept: the roots, with \p Owners the nodes that this
/// object file owns, and what those refer to.
void Scanner::markKept(const OwnerTable *Owners) {
  for (Node &N : Nodes)
    N.IsKept = N.IsKeptWhole = false;
  for (uint32_t I = 0; I != Nodes.size(); ++I) {
    const Node &N = Nodes[I];
    if ((N.IsRoot && !N.IsContainer) ||
        (Owners && N.IsShared &&
         Owners->lookup(N.Identity) == OwnerTable::owner(Index, N.Begin)))
      keep(I, /*Whole=*/true);
  }
  keepReferenced(Owners);
}

void Scanner::recordOwners(OwnerTable &Owners) const {
  for (const Node &N : Nodes)
    if (N.IsKept && N.IsShared)
      Owners.record(N.Identity, Index, N.Begin);
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
  Info.FirstLocList = Object.LocListOffsets.size();
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
/// object's range lists. Returns whether any of its code is in the binary.
bool Scanner::addRangeList(const UnitInfo &Unit, uint64_t Offset,
                           const AddressRange *Function) {
  uint32_t ListOffset = Object.RangesData.size();
  Object.RangeListOffsets.push_back(ListOffset);
  unsigned Size = Unit.AddressSize;
  uint64_t MaxAddress = Size == 8 ? UINT64_MAX : UINT32_MAX;
  uint64_t Base = Unit.InLowPc;
  StringRef Section = Object.Ranges;
  // The entry to add next, unless the entry after it continues it.
  uint64_t PendingBegin = 0, PendingEnd = 0;
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
    if (PendingBegin != PendingEnd && Linked == PendingEnd) {
      PendingEnd = Linked + (End - Begin);
      continue;
    }
    if (PendingBegin != PendingEnd)
      addRangeListEntry(Unit, PendingBegin, PendingEnd);
    PendingBegin = Linked;
    PendingEnd = Linked + (End - Begin);
  }
  if (PendingBegin != PendingEnd)
    addRangeListEntry(Unit, PendingBegin, PendingEnd);
  bool HasCode = Object.RangesData.size() != ListOffset;
  addRangeListEntry(Unit, outBaseAddress(Object, Unit),
                    outBaseAddress(Object, Unit));
  return HasCode;
}

/// Adds \p Str to the object's strings and returns its offset there.
uint32_t Scanner::addString(StringRef Str) {
  uint32_t Offset = Object.Str.size() + Object.ExtraStrings.size();
  Object.ExtraStrings.append(Str.begin(), Str.end());
  Object.ExtraStrings.push_back('\0');
  return Offset;
}

/// Takes from an attribute of an entry what it says about the entry's names.
/// \p UnitOffset is where the entry's unit is in __debug_info.
void Scanner::noteName(EntryNames &Names, AttributeSpec Spec,
                       const uint8_t *Value, uint64_t Size,
                       uint32_t UnitOffset) {
  switch (Spec.Attribute) {
  case dwarf::DW_AT_name:
  case dwarf::DW_AT_linkage_name:
  case dwarf::DW_AT_MIPS_linkage_name: {
    uint32_t Offset = NoString;
    if (Spec.Form == dwarf::DW_FORM_strp)
      Offset = read32le(Value);
    else if (Spec.Form == dwarf::DW_FORM_string)
      Offset = addString(reinterpret_cast<const char *>(Value));
    (Spec.Attribute == dwarf::DW_AT_name ? Names.Name : Names.LinkageName) =
        Offset;
    break;
  }
  case dwarf::DW_AT_specification:
  case dwarf::DW_AT_abstract_origin:
    // If an entry has both, dsymutil looks at what it specifies.
    if (isReference(Spec.Form) &&
        (!Names.Origin || Spec.Attribute == dwarf::DW_AT_specification))
      Names.Origin = readReference(Value, Size) +
                     (Spec.Form == dwarf::DW_FORM_ref_addr ? 0 : UnitOffset);
    break;
  default:
    break;
  }
}

/// Reads the names of the entry at \p Offset of __debug_info, which can be
/// in any unit and does not have to be kept.
EntryNames Scanner::readNames(uint32_t Offset) {
  auto It = llvm::upper_bound(
      Units, Offset,
      [](uint32_t Offset, const ScannedUnit &U) { return Offset < U.Offset; });
  if (It == Units.begin())
    return EntryNames();
  const ScannedUnit &Unit = *std::prev(It);
  const uint8_t *Base = Object.Info.bytes_begin();
  const uint8_t *P = Base + Offset;
  const uint8_t *End = Base + Unit.Offset + Unit.Header.Size;
  if (Offset < Unit.Offset + Unit.Header.HeaderSize || P >= End)
    return EntryNames();

  const AbbreviationTable *Table = &Abbreviations;
  if (Unit.Header.AbbrevOffset != AbbrevOffset) {
    // The scan has parsed this table before, successfully.
    if (Unit.Header.AbbrevOffset != OriginAbbrevOffset)
      OriginAbbreviations.parse(Object.Abbrev, Unit.Header.AbbrevOffset);
    OriginAbbrevOffset = Unit.Header.AbbrevOffset;
    Table = &OriginAbbreviations;
  }
  const Abbreviation *Abbrev = Table->find(readULEB(P));
  if (!Abbrev)
    return EntryNames();
  EntryNames Names;
  for (AttributeSpec Spec : Table->attributes(*Abbrev)) {
    uint64_t Size =
        formSize(Spec.Form, P, Unit.Header.Version, Unit.Header.AddressSize);
    // A reference can be wrong, and point at something else than an entry.
    if (Size > uint64_t(End - P))
      return EntryNames();
    noteName(Names, Spec, P, Size, Unit.Offset);
    P += Size;
  }
  return Names;
}

/// Gives \p Names the names that the entry does not have itself and the
/// entry that it specifies or is an instance of has: an inlined copy of a
/// function has its name there.
void Scanner::completeNames(EntryNames &Names) {
  if (!Names.Origin || Names.Origin >= Object.Info.size() ||
      (Names.Name != NoString && Names.LinkageName != NoString))
    return;
  // Many entries are instances of the same entry: read that one once.
  auto [It, Inserted] = OriginNames.try_emplace(Names.Origin);
  if (Inserted) {
    EntryNames Origin;
    Origin.Origin = Names.Origin;
    // The limit is for references that go in circles.
    for (unsigned Depth = 0;
         Depth != 8 && Origin.Origin &&
         (Origin.Name == NoString || Origin.LinkageName == NoString);
         ++Depth) {
      EntryNames Next = readNames(Origin.Origin);
      if (Origin.Name == NoString)
        Origin.Name = Next.Name;
      if (Origin.LinkageName == NoString)
        Origin.LinkageName = Next.LinkageName;
      Origin.Origin = Next.Origin;
    }
    It->second = {Origin.Name, Origin.LinkageName};
    // These strings need not be those of a kept entry.
    for (uint32_t Name : {Origin.Name, Origin.LinkageName})
      if (Name != NoString)
        Object.StringOffsets.push_back(Name);
  }
  if (Names.Name == NoString)
    Names.Name = It->second.first;
  if (Names.LinkageName == NoString)
    Names.LinkageName = It->second.second;
}

/// Makes sure that the string at \p Name, a name of an entry, gets into the
/// output. The strings of the kept entries, and of the entries that those
/// are instances of, do anyway; the extra strings do not.
void Scanner::useExtraString(uint32_t Name) {
  if (Name >= Object.Str.size())
    Object.StringOffsets.push_back(Name);
}

/// Adds to \p Table that the entry at \p Entry of the object's contribution
/// to __debug_info is found by the name \p Name.
void Scanner::addName(std::vector<AccelName> &Table, uint32_t Name,
                      uint32_t Entry) {
  Table.push_back({Name, Entry});
  useExtraString(Name);
}

/// Adds the names of an entry that describes code: a function, an inlined
/// copy of one, or a label. They are the names that dsymutil gives it.
void Scanner::addCodeNames(EntryNames Names, uint16_t Tag, uint32_t Entry) {
  completeNames(Names);
  // An empty name is as good as none.
  StringRef Name = Names.Name == NoString ? "" : Object.string(Names.Name);
  StringRef LinkageName =
      Names.LinkageName == NoString ? "" : Object.string(Names.LinkageName);
  bool HasLinkageName = !LinkageName.empty() && LinkageName != Name;
  if (HasLinkageName)
    addName(Object.Names, Names.LinkageName, Entry);
  if (Name.empty())
    return;
  // "f<int>" is also found as "f".
  if (HasLinkageName && Tag != dwarf::DW_TAG_inlined_subroutine)
    if (std::optional<StringRef> Stripped = StripTemplateParameters(Name))
      addName(Object.Names, addString(*Stripped), Entry);
  addName(Object.Names, Names.Name, Entry);
  // An Objective-C method "-[Class(Category) selector]" is also found by its
  // selector and without the category, and its class leads to it.
  if (std::optional<ObjCSelectorNames> ObjC = getObjCNamesIfSelector(Name)) {
    addName(Object.Names, addString(ObjC->Selector), Entry);
    addName(Object.ObjCClasses, addString(ObjC->ClassName), Entry);
    if (ObjC->ClassNameNoCategory)
      addName(Object.ObjCClasses, addString(*ObjC->ClassNameNoCategory), Entry);
    if (ObjC->MethodNameNoCategory)
      addName(Object.Names, addString(*ObjC->MethodNameNoCategory), Entry);
  }
}

/// The hash of \p Name qualified with the names of the scopes it is in, of
/// which \p Parent is the innermost: what dsymutil puts into the table of
/// types, and what lldb compares with the hash of "A::B::Name" ("::Name" if
/// the name is in no scope). \p IsScope says that the name is that of a
/// scope itself.
static uint32_t hashQualifiedName(StringRef Name, const Scope &Parent,
                                  bool IsScope) {
  if (Parent.EndsQualifiedNames)
    return djbHash(Name, djbHash(IsScope ? "" : "::"));
  // A scope without a name is left out.
  return djbHash(Name,
                 djbHash(Name.empty() ? "" : "::", Parent.QualifiedNameHash));
}

/// Translates the location list at \p Offset of __debug_loc, which belongs
/// to an entry in \p Function, and adds it to the object's location lists.
/// If \p Function is null, the entry is not in code of the binary, and its
/// list is empty.
void Scanner::addLocationList(const UnitInfo &Unit, uint64_t Offset,
                              const AddressRange *Function) {
  Object.LocListOffsets.push_back(Object.LocData.size());
  unsigned Size = Unit.AddressSize;
  uint64_t MaxAddress = Size == 8 ? UINT64_MAX : UINT32_MAX;
  uint64_t Base = Unit.InLowPc;
  StringRef Section = Object.Loc;
  auto Append = [&](uint64_t Value, unsigned Size) {
    char Bytes[8];
    writeUnsigned(reinterpret_cast<uint8_t *>(Bytes), Size, Value);
    Object.LocData.append(Bytes, Bytes + Size);
  };
  // An entry is two addresses, the length of an expression, and the
  // expression. Two zeros end the list.
  while (Function && Offset <= Section.size() &&
         Section.size() - Offset >= 2 * Size) {
    uint64_t Begin = readAddress(Section.bytes_begin() + Offset, Size);
    uint64_t End = readAddress(Section.bytes_begin() + Offset + Size, Size);
    Offset += 2 * Size;
    if (Begin == 0 && End == 0)
      break;
    if (Begin == MaxAddress) {
      Base = End;
      continue;
    }
    if (Section.size() - Offset < 2)
      break;
    uint64_t Length = read16le(Section.bytes_begin() + Offset);
    Offset += 2;
    if (Length > Section.size() - Offset)
      break;
    // Like dsymutil, move the entry with the function, whatever it covers.
    uint64_t Moved = Base + Function->Linked - Function->Begin -
                     outBaseAddress(Object, Unit);
    Append(Begin + Moved, Size);
    Append(End + Moved, Size);
    Append(Length, 2);
    Object.LocData.append(Section.begin() + Offset,
                          Section.begin() + Offset + Length);
    patchExpression(
        Object,
        MutableArrayRef(
            reinterpret_cast<uint8_t *>(Object.LocData.end() - Length), Length),
        Size, NoOffset, /*IsVariable=*/false);
    Offset += Length;
  }
  Append(0, Size);
  Append(0, Size);
}

/// Gathers what the layout needs to know about the kept entries of \p Unit:
/// the strings they use, their range and location lists, which are translated
/// here, which of their references leave the unit, and the names they are
/// found by.
void Scanner::collectFromKeptEntries(const UnitInfo &Unit) {
  Abbreviations.parse(Object.Abbrev, Unit.AbbrevOffset);
  AbbrevOffset = Unit.AbbrevOffset;
  const uint8_t *Base = Object.Info.bytes_begin();
  // The kept entries are a tree like the unit was. These are the entries
  // that the current one is in, innermost last.
  SmallVector<Scope, 16> Scopes;
  for (const KeptRange &Range :
       ArrayRef(Object.KeptRanges).slice(Unit.FirstRange, Unit.NumRanges)) {
    const uint8_t *P = Base + Range.InBegin;
    const uint8_t *End = Base + Range.InEnd;
    while (P < End) {
      uint32_t Entry = Range.Out + (P - Base - Range.InBegin);
      uint64_t Code = readULEB(P);
      if (Code == 0) {
        Scopes.pop_back();
        continue;
      }
      const Abbreviation &Abbrev = *Abbreviations.find(Code);
      bool IsUnitEntry = Scopes.empty();
      // The function that the entry is in, or is, if it is in the binary.
      const AddressRange *Function =
          IsUnitEntry || Abbrev.Tag == dwarf::DW_TAG_subprogram
              ? nullptr
              : Scopes.back().Function;
      EntryNames Names;
      // Whether the entry describes code that is in the binary. As in
      // dsymutil, everything with addresses in such a function does.
      bool HasCode = false;
      bool IsDeclaration = false;
      bool IsObjCClass = false;
      bool IsObjCImplementation = false;
      bool InFunction = !IsUnitEntry && Scopes.back().InFunction;
      // Whether the entry is a variable that dsymutil has in the table of
      // names: a constant outside of functions, or a variable at an address
      // that is in the binary.
      bool IsVariable = Abbrev.Tag == dwarf::DW_TAG_variable ||
                        Abbrev.Tag == dwarf::DW_TAG_constant;
      bool IsNamedVariable = false;

      for (AttributeSpec Spec : Abbreviations.attributes(Abbrev)) {
        const uint8_t *Value = P;
        uint64_t Size = formSize(Spec.Form, P, Unit.Version, Unit.AddressSize);
        P += Size;
        if (Spec.Form == dwarf::DW_FORM_strp)
          Object.StringOffsets.push_back(read32le(Value));
        // A reference to an entry of another unit cannot stay relative to
        // this one. All attributes with the abbreviation follow.
        else if (Spec.Form == dwarf::DW_FORM_ref4 && Unit.Version >= 3 &&
                 Object.findRedirect(Unit.InOffset + read32le(Value)))
          Object.RefAddrForms.push_back(Spec.FormOffset);

        switch (Spec.Attribute) {
        case dwarf::DW_AT_name:
        case dwarf::DW_AT_linkage_name:
        case dwarf::DW_AT_MIPS_linkage_name:
        case dwarf::DW_AT_specification:
        case dwarf::DW_AT_abstract_origin:
          noteName(Names, Spec, Value, Size, Unit.InOffset);
          break;
        case dwarf::DW_AT_low_pc: {
          if (Spec.Form != dwarf::DW_FORM_addr)
            break;
          const AddressRange *Code =
              Function
                  ? Function
                  : Object.findContaining(readAddress(Value, Unit.AddressSize));
          HasCode = Code != nullptr;
          if (Abbrev.Tag == dwarf::DW_TAG_subprogram)
            Function = Code;
          break;
        }
        case dwarf::DW_AT_ranges:
          if (!isSectionOffset(Spec.Form, Unit.Version))
            break;
          if (!IsUnitEntry) {
            HasCode |=
                addRangeList(Unit, read32le(Value), Function) || Function;
            break;
          }
          // The unit's own list is that of its functions.
          Object.RangeListOffsets.push_back(Object.RangesData.size());
          for (std::pair<uint64_t, uint64_t> R :
               ArrayRef(Object.UnitRanges)
                   .slice(Unit.FirstAddressRange, Unit.NumAddressRanges))
            addRangeListEntry(Unit, R.first, R.second);
          addRangeListEntry(Unit, outBaseAddress(Object, Unit),
                            outBaseAddress(Object, Unit));
          break;
        case dwarf::DW_AT_declaration:
          IsDeclaration = readFlag(Spec.Form, Value);
          break;
        case dwarf::DW_AT_const_value:
          IsNamedVariable |= IsVariable && !InFunction;
          break;
        case dwarf::DW_AT_location:
        case dwarf::DW_AT_string_length:
        case dwarf::DW_AT_return_addr:
        case dwarf::DW_AT_data_member_location:
        case dwarf::DW_AT_frame_base:
        case dwarf::DW_AT_static_link:
        case dwarf::DW_AT_segment:
        case dwarf::DW_AT_use_location:
        case dwarf::DW_AT_vtable_elem_location:
          if (isLocationList(Spec, Unit.Version)) {
            addLocationList(Unit, read32le(Value), Function);
          } else if (IsVariable && Spec.Attribute == dwarf::DW_AT_location &&
                     isBlock(Spec.Form)) {
            // A pointer to the address, as opposed to a variable at it, does
            // not count.
            ArrayRef<uint8_t> Location = blockContents(Spec.Form, Value);
            forEachAddressOperand(
                Location, Unit.AddressSize, [&](size_t Offset, unsigned Size) {
                  if (Object.findOperand(
                          readReference(Location.data() + Offset, Size),
                          Location.data() + Offset - Base))
                    IsNamedVariable |=
                        isMemoryLocation(Location, Unit.AddressSize);
                });
          }
          break;
        case dwarf::DW_AT_APPLE_runtime_class: {
          uint64_t Language = readUnsigned(Spec.Form, Value, Size);
          IsObjCClass = Language == dwarf::DW_LANG_ObjC ||
                        Language == dwarf::DW_LANG_ObjC_plus_plus;
          break;
        }
        case dwarf::DW_AT_APPLE_objc_complete_type:
          IsObjCImplementation = readFlag(Spec.Form, Value);
          break;
        default:
          break;
        }
      }

      // The accelerator tables get what dsymutil puts into them.
      if (IsUnitEntry) {
        Scopes.push_back({0, /*EndsQualifiedNames=*/true, nullptr, false});
        continue;
      }
      if (HasCode || IsNamedVariable) {
        if (Abbrev.Tag != dwarf::DW_TAG_lexical_block)
          addCodeNames(Names, Abbrev.Tag, Entry);
      } else if (Abbrev.Tag == dwarf::DW_TAG_namespace) {
        if (Names.Name == NoString && AnonymousNamespace == NoString)
          AnonymousNamespace = addString("(anonymous namespace)");
        addName(Object.Namespaces,
                Names.Name != NoString ? Names.Name : AnonymousNamespace,
                Entry);
      } else if (Abbrev.Tag == dwarf::DW_TAG_imported_declaration) {
        // An alias of a namespace.
        if (Names.Name != NoString)
          addName(Object.Namespaces, Names.Name, Entry);
      } else if (isTypeTag(Abbrev.Tag) && !IsDeclaration) {
        EntryNames TypeNames = Names;
        completeNames(TypeNames);
        StringRef Name =
            TypeNames.Name == NoString ? "" : Object.string(TypeNames.Name);
        if (!Name.empty()) {
          Object.Types.push_back(
              {TypeNames.Name, Entry,
               hashQualifiedName(Name, Scopes.back(), /*IsScope=*/false),
               Abbrev.Tag, IsObjCClass && IsObjCImplementation});
          useExtraString(TypeNames.Name);
        }
      }

      if (!Abbrev.HasChildren)
        continue;
      if (Abbrev.Tag == dwarf::DW_TAG_module) {
        Scopes.push_back({0, /*EndsQualifiedNames=*/true, nullptr, false});
        continue;
      }
      // dsymutil also takes the name of a function from the entry that it
      // specifies. lldb leaves functions out of the names that it looks for,
      // so a type in a function is not found by this hash either way.
      StringRef Name;
      if (Names.Name != NoString)
        Name = Object.string(Names.Name);
      else if (Abbrev.Tag == dwarf::DW_TAG_namespace)
        Name = "(anonymous namespace)";
      Scopes.push_back(
          {hashQualifiedName(Name, Scopes.back(), /*IsScope=*/true),
           /*EndsQualifiedNames=*/false, Function,
           InFunction || Abbrev.Tag == dwarf::DW_TAG_subprogram});
    }
  }
}

void Scanner::scan() {
  if (Object.Info.empty())
    return;
  if (Object.Info.size() >= RedirectableRef) {
    Object.Warning = "__debug_info is too large";
    return;
  }
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
      Nodes.clear();
      break;
    }
    Unit.EndNode = Nodes.size();
    Unit.EndFunctionRange = FunctionRanges.size();
    Offset += Unit.Header.Size;
  }
  // The passes that follow parse again what they need.
  Abbreviations = AbbreviationTable();
  StringHashes = DenseMap<uint32_t, uint64_t>();
  if (Nodes.size() >= NoNode) {
    Object.Warning = "__debug_info has too many entries";
    Nodes.clear();
  }
  // Like dsymutil, take nothing from an object file that has no code and no
  // variable in the binary.
  if (FunctionRanges.empty() && !HasLiveVariable)
    Nodes.clear();
  if (Nodes.empty())
    return;

  markKept(nullptr);
  for (const Node &N : Nodes)
    NumIdentities += N.IsKept && N.IsShared;
}

void Scanner::resolve(const OwnerTable &Owners) {
  if (Nodes.empty())
    return;
  markKept(&Owners);
  llvm::sort(Object.Redirects, [](const Redirect &A, const Redirect &B) {
    return A.InOffset < B.InOffset;
  });
  Object.Redirects.erase(llvm::unique(Object.Redirects,
                                      [](const Redirect &A, const Redirect &B) {
                                        return A.InOffset == B.InOffset;
                                      }),
                         Object.Redirects.end());

  for (ScannedUnit &Unit : Units)
    addRanges(Unit);
  llvm::sort(Object.StringOffsets);
  Object.StringOffsets.erase(llvm::unique(Object.StringOffsets),
                             Object.StringOffsets.end());
  llvm::sort(Object.RefAddrForms);
  Object.RefAddrForms.erase(llvm::unique(Object.RefAddrForms),
                            Object.RefAddrForms.end());
  if (const char *Problem = linkFrameTable(Object))
    Object.Warning = Problem;
}

void DwarfLinker::scan() {
  std::vector<std::unique_ptr<Scanner>> Scanners(Objects.size());
  {
    PhaseTimer Timer("scan");
    parallelFor(0, Objects.size(), [&](size_t I) {
      Scanners[I] = std::make_unique<Scanner>(Objects[I], I);
      Scanners[I]->scan();
    });
  }
  size_t NumIdentities = 0;
  for (const std::unique_ptr<Scanner> &S : Scanners)
    NumIdentities += S->numIdentities();
  OwnerTable Owners(NumIdentities);
  {
    PhaseTimer Timer("find owners");
    parallelFor(0, Objects.size(),
                [&](size_t I) { Scanners[I]->recordOwners(Owners); });
  }
  {
    PhaseTimer Timer("resolve");
    parallelFor(0, Objects.size(), [&](size_t I) {
      Scanners[I]->resolve(Owners);
      Scanners[I].reset();
    });
  }
}

//===----------------------------------------------------------------------===//
// Layout.
//===----------------------------------------------------------------------===//

void DwarfLinker::mergeStrings() {
  StrSize = dsymutil2::mergeStrings(Objects);
}

void DwarfLinker::layout(DsymWriter &Writer) {
  // Where each distinct CIE is in the output's __debug_frame. The first
  // object to use a CIE writes it.
  DenseMap<StringRef, uint32_t> CIEOffsets;
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
    Object.LocOffset = LocSize;
    LocSize += Object.LocData.size();
    Object.ArangesOffset = ArangesSize;
    ArangesSize += Object.ArangesSize;
    Object.FrameOffset = FrameSize;
    for (StringRef CIE : Object.FrameCIEs) {
      auto [It, Inserted] = CIEOffsets.try_emplace(CIE, FrameSize);
      if (Inserted)
        FrameSize += CIE.size();
      Object.FrameCIEOffsets.push_back(It->second);
    }
    Object.FrameFDEOffset = FrameSize;
    FrameSize += Object.FrameData.size();
  }
  // Offsets into these sections are 32 bits wide in DWARF32.
  if (InfoSize > UINT32_MAX || AbbrevSize > UINT32_MAX ||
      LineSize > UINT32_MAX || RangesSize > UINT32_MAX ||
      LocSize > UINT32_MAX || FrameSize > UINT32_MAX)
    fatal("the debug info does not fit in 4 GB sections");
  if (InfoSize == 0)
    return;
  {
    PhaseTimer Timer("accelerator tables");
    AccelTables[0] = buildNameTable(Objects, &ObjectFile::Names);
    AccelTables[1] = buildNameTable(Objects, &ObjectFile::Namespaces);
    AccelTables[2] = buildTypeTable(Objects);
    AccelTables[3] = buildNameTable(Objects, &ObjectFile::ObjCClasses);
  }
  // The sections are in the order in which write() completes them: the
  // writer writes a section to the file while the ones after it are filled.
  // The tables are complete. lldb uses them only if it has all of them.
  static const char *const AccelNames[] = {"__apple_names", "__apple_namespac",
                                           "__apple_types", "__apple_objc"};
  for (size_t I = 0; I != std::size(AccelTables); ++I)
    Writer.addSection(AccelNames[I], AccelTables[I]);
  if (LineSize)
    LineSection = Writer.addSection("__debug_line", LineSize);
  if (RangesSize)
    RangesSection = Writer.addSection("__debug_ranges", RangesSize);
  if (LocSize)
    LocSection = Writer.addSection("__debug_loc", LocSize);
  if (FrameSize)
    FrameSection = Writer.addSection("__debug_frame", FrameSize);
  if (StrSize)
    StrSection = Writer.addSection("__debug_str", StrSize);
  InfoSection = Writer.addSection("__debug_info", InfoSize);
  AbbrevSection = Writer.addSection("__debug_abbrev", AbbrevSize);
  if (ArangesSize)
    ArangesSection = Writer.addSection("__debug_aranges", ArangesSize);
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
  UnitWriter(ArrayRef<ObjectFile> Objects, const ObjectFile &Object,
             uint8_t *Out)
      : Objects(Objects), Object(Object), Out(Out) {}

  void write(const UnitInfo &Unit);
  /// Writes the unit's entry in __debug_aranges to \p Out and returns its
  /// size.
  uint64_t writeAranges(const UnitInfo &Unit, uint8_t *Out) const;

private:
  void patchRange(const UnitInfo &Unit, const KeptRange &Range);
  uint64_t sectionOffset(uint32_t InOffset) const;

  ArrayRef<ObjectFile> Objects;
  const ObjectFile &Object;
  uint8_t *Out;
  AbbreviationTable Abbreviations;
  /// The live function that the entries being patched are in, if any.
  const AddressRange *Function = nullptr;
  /// Whether the next entry is the unit's own.
  bool AtUnitEntry = false;
  /// The next of the unit's range lists in ObjectFile::RangeListOffsets, and
  /// of its location lists in ObjectFile::LocListOffsets.
  uint32_t NextRangeList = 0;
  uint32_t NextLocList = 0;
};
} // namespace

/// Where a reference to the entry at \p InOffset of the object's
/// __debug_info points to in the output's __debug_info: the entry's copy
/// here, or the copy of the object file that owns the entry.
uint64_t UnitWriter::sectionOffset(uint32_t InOffset) const {
  if (const Redirect *R = Object.findRedirect(InOffset)) {
    const ObjectFile &Owner = Objects[R->OwnerObject];
    return Owner.InfoOffset + Owner.infoOutOffset(R->OwnerOffset);
  }
  return Object.InfoOffset + Object.infoOutOffset(InOffset);
}

void UnitWriter::write(const UnitInfo &Unit) {
  uint8_t *Header = Out + Unit.OutOffset;
  memcpy(Header, Object.Info.data() + Unit.InOffset, Unit.HeaderSize);
  write32le(Header, Unit.OutSize - 4);
  write32le(Header + 6, Object.AbbrevOffset + Unit.AbbrevOffset);

  // The scan has parsed this table before, successfully.
  Abbreviations.parse(Object.Abbrev, Unit.AbbrevOffset);
  for (AttributeSpec &Spec : Abbreviations.allAttributes())
    if (Spec.Form == dwarf::DW_FORM_ref4 &&
        llvm::binary_search(Object.RefAddrForms, Spec.FormOffset))
      Spec.Form = FormRef4AsRefAddr;
  AtUnitEntry = true;
  NextRangeList = Unit.FirstRangeList;
  NextLocList = Unit.FirstLocList;
  for (const KeptRange &Range :
       ArrayRef(Object.KeptRanges).slice(Unit.FirstRange, Unit.NumRanges)) {
    memcpy(Out + Range.Out, Object.Info.data() + Range.InBegin,
           Range.InEnd - Range.InBegin);
    patchRange(Unit, Range);
  }
}

/// Patches the entries of the copy of \p Range.
void UnitWriter::patchRange(const UnitInfo &Unit, const KeptRange &Range) {
  uint8_t *P = Out + Range.Out;
  uint8_t *End = P + (Range.InEnd - Range.InBegin);
  // Where the copy is, relative to the original.
  const uint8_t *Original = Object.Info.bytes_begin() + Range.InBegin;
  const uint8_t *Copy = P;
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
          else if (isLocationList(Spec, Unit.Version))
            write32le(P,
                      Object.LocOffset + Object.LocListOffsets[NextLocList++]);
          break;
        case dwarf::DW_FORM_exprloc:
        case dwarf::DW_FORM_block:
        case dwarf::DW_FORM_block1:
        case dwarf::DW_FORM_block2:
        case dwarf::DW_FORM_block4: {
          ArrayRef<uint8_t> Expression = blockContents(Spec.Form, P);
          patchExpression(
              Object,
              MutableArrayRef(const_cast<uint8_t *>(Expression.data()),
                              Expression.size()),
              Unit.AddressSize,
              Expression.data() - Copy + Original - Object.Info.bytes_begin(),
              Entry->Tag == dwarf::DW_TAG_variable ||
                  Entry->Tag == dwarf::DW_TAG_constant);
          break;
        }
        case dwarf::DW_FORM_strp:
          write32le(P, Object.stringOutOffset(read32le(P)));
          break;
        case dwarf::DW_FORM_ref1:
        case dwarf::DW_FORM_ref2:
        case dwarf::DW_FORM_ref4:
        case dwarf::DW_FORM_ref8: {
          // Relative to the unit. Entries only move towards the start of
          // their unit, so the new value fits.
          uint64_t Target =
              Object.infoOutOffset(Unit.InOffset + readReference(P, Size)) -
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
        case FormRef4AsRefAddr:
          // Relative to the unit in the object file, to the section here.
          write32le(P, sectionOffset(Unit.InOffset + read32le(P)));
          break;
        case dwarf::DW_FORM_ref_addr: {
          // Relative to the section.
          uint64_t Target = sectionOffset(readReference(P, Size));
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
  // First the sections whose contents the scan has made, so that the writer
  // has something to write to the file while the others are filled.
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    if (!Object.LineData.empty())
      memcpy(Writer.sectionData(LineSection) + Object.LineOffset,
             Object.LineData.data(), Object.LineData.size());
    if (!Object.RangesData.empty())
      memcpy(Writer.sectionData(RangesSection) + Object.RangesOffset,
             Object.RangesData.data(), Object.RangesData.size());
    if (!Object.LocData.empty())
      memcpy(Writer.sectionData(LocSection) + Object.LocOffset,
             Object.LocData.data(), Object.LocData.size());
    if (!Object.FrameData.empty()) {
      char *Frame = Writer.sectionData(FrameSection);
      for (size_t I = 0; I != Object.FrameCIEs.size(); ++I)
        if (Object.FrameCIEOffsets[I] >= Object.FrameOffset)
          memcpy(Frame + Object.FrameCIEOffsets[I], Object.FrameCIEs[I].data(),
                 Object.FrameCIEs[I].size());
      memcpy(Frame + Object.FrameFDEOffset, Object.FrameData.data(),
             Object.FrameData.size());
      patchFrameTable(Object, Frame + Object.FrameFDEOffset);
    }
  });
  if (LineSize)
    Writer.finishSection(LineSection);
  if (RangesSize)
    Writer.finishSection(RangesSection);
  if (LocSize)
    Writer.finishSection(LocSection);
  if (FrameSize)
    Writer.finishSection(FrameSection);

  if (StrSize) {
    writeStrings(Objects, Writer.sectionData(StrSection));
    Writer.finishSection(StrSection);
  }

  char *Info = Writer.sectionData(InfoSection);
  char *Abbrev = Writer.sectionData(AbbrevSection);
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    if (Object.Units.empty())
      return;
    UnitWriter Units(Objects, Object,
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
    memcpy(Abbrev + Object.AbbrevOffset, Object.Abbrev.data(),
           Object.Abbrev.size());
    for (uint32_t FormOffset : Object.RefAddrForms)
      Abbrev[Object.AbbrevOffset + FormOffset] = dwarf::DW_FORM_ref_addr;
  });
}
