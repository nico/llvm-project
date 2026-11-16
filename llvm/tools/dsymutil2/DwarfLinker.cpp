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
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/bit.h"
#include "llvm/DebugInfo/DWARF/DWARFAcceleratorTable.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
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
  /// DWARF 5: where the unit's own entry says the unit's table of string
  /// offsets is in __debug_str_offs, and its table of addresses in
  /// __debug_addr. 0 if it does not say.
  uint32_t StrOffsetsBase = 0;
  uint32_t AddrBase = 0;
  /// The same for the tables of the offsets of its range lists and of its
  /// location lists, in __debug_rnglists and __debug_loclists.
  uint32_t RngListsBase = 0;
  uint32_t LocListsBase = 0;
  /// See UnitInfo.
  uint32_t LowPcOffset = 0;
  uint32_t LowPcFormOffset = 0;
  uint8_t LowPcSize = 0;
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
  if (Header.Version < 2 || Header.Version > 5)
    return "unsupported DWARF version";
  if (Header.Version == 5) {
    // DWARF 5 has the fields in another order, after the kind of unit. The
    // other kinds have more fields; compilers do not make them for Mach-O.
    if (Available < 12)
      return "truncated compile unit";
    if (P[6] != dwarf::DW_UT_compile && P[6] != dwarf::DW_UT_partial)
      return "unsupported kind of unit";
    Header.AddressSize = P[7];
    Header.AbbrevOffset = read32le(P + 8);
    Header.HeaderSize = 12;
  } else {
    Header.AbbrevOffset = read32le(P + 6);
    Header.AddressSize = P[10];
    Header.HeaderSize = 11;
  }
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

constexpr uint32_t NoString = UINT32_MAX;

/// The offset in the strings of \p Object of the string that the index at
/// \p P of form \p Form refers to, or NoString if there is no such slot in
/// the table at \p StrOffsetsBase.
static uint32_t indexedStringOffset(const ObjectFile &Object, uint16_t Form,
                                    const uint8_t *P, uint32_t StrOffsetsBase) {
  uint64_t Slot = StrOffsetsBase + 4 * readIndex(Form, P);
  if (StrOffsetsBase == 0 || Slot > Object.StrOffsets.size() ||
      Object.StrOffsets.size() - Slot < 4)
    return NoString;
  return read32le(Object.StrOffsets.bytes_begin() + Slot);
}

/// The offset in the strings of \p Object of the string that the value at
/// \p P of form \p Form refers to, or NoString if it refers to none.
/// \p StrOffsetsBase is that of the value's unit.
LLVM_ATTRIBUTE_ALWAYS_INLINE
static uint32_t stringOffset(const ObjectFile &Object, uint16_t Form,
                             const uint8_t *P, uint32_t StrOffsetsBase) {
  if (Form == dwarf::DW_FORM_strp)
    return read32le(P);
  return isStringIndex(Form)
             ? indexedStringOffset(Object, Form, P, StrOffsetsBase)
             : NoString;
}

/// Not an offset in a section.
constexpr uint64_t NoOffset = UINT64_MAX;

/// Where slot \p Index of the table of addresses at \p AddrBase is in
/// __debug_addr, or NoOffset if there is no such slot.
static uint64_t addressSlot(const ObjectFile &Object, uint64_t Index,
                            unsigned AddressSize, uint32_t AddrBase) {
  uint64_t Size = Object.Addr.size();
  if (AddrBase == 0 || AddrBase > Size ||
      Index >= (Size - AddrBase) / AddressSize)
    return NoOffset;
  return AddrBase + Index * AddressSize;
}

/// The address in the object file that the value at \p P of the form
/// \p Form says: one that isAddress(). The scan has checked that the table
/// has the slot of an index.
LLVM_ATTRIBUTE_ALWAYS_INLINE
static uint64_t addressValue(const ObjectFile &Object, uint16_t Form,
                             const uint8_t *P, unsigned AddressSize,
                             uint32_t AddrBase) {
  if (LLVM_LIKELY(Form == dwarf::DW_FORM_addr))
    return readAddress(P, AddressSize);
  return readAddress(Object.Addr.bytes_begin() + AddrBase +
                         AddressSize * readIndex(Form, P),
                     AddressSize);
}

/// Where the list with the index \p Index is in \p Section, whose table of
/// the offsets of lists is at \p Base, or NoOffset if there is no such slot.
static uint64_t indexedList(StringRef Section, uint32_t Base, uint64_t Index) {
  if (Base == 0 || Base > Section.size() ||
      Index >= (Section.size() - Base) / 4)
    return NoOffset;
  return Base + uint64_t(read32le(Section.bytes_begin() + Base + 4 * Index));
}

/// The symbol that the operand at \p Operand of a location expression is
/// the address of, or null, and in \p Address the address in the object
/// file. \p Size and \p IsIndex are what forEachAddressOperand() says about
/// the operand, \p InfoOffset where it is in __debug_info, or NoOffset, and
/// \p AddrBase where its unit's table of addresses is.
static const AddressRange *findOperand(const ObjectFile &Object,
                                       const uint8_t *Operand, unsigned Size,
                                       bool IsIndex, unsigned AddressSize,
                                       uint32_t AddrBase, uint64_t InfoOffset,
                                       uint64_t &Address) {
  if (!IsIndex) {
    Address = readReference(Operand, Size);
    return Object.findOperand(Address, InfoOffset);
  }
  uint64_t Slot = addressSlot(Object, readULEB(Operand), AddressSize, AddrBase);
  if (Slot == NoOffset)
    return nullptr;
  Address = readAddress(Object.Addr.bytes_begin() + Slot, AddressSize);
  return Object.findSlot(Address, Slot);
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
/// of a location list in __debug_loc or, in DWARF 5, in __debug_loclists.
/// There, most are indexes instead: DW_FORM_loclistx.
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

/// Turns the addresses in the location expression \p Expression from those
/// of the object file into those of the binary. \p InfoOffset is where the
/// expression is in the object's __debug_info, or NoOffset. An address that
/// is an index into the table at \p AddrBase becomes the index that its
/// slot has in \p Addresses, the output's table, if that is given. If
/// \p Unit is given, the expression is in that unit, and where it names a
/// base type by the offset of its entry in the unit, that becomes the offset
/// in the output: the entry is kept, like all base types, and the kept
/// entries of the unit are known.
///
/// An address that is not in the binary becomes 0, as in dsymutil. If
/// \p IsVariable, the expression is the location of a variable, which then
/// has none: dsymutil removes the attribute, and here the expression is
/// overwritten with operations that do nothing.
static void patchExpression(const ObjectFile &Object,
                            MutableArrayRef<uint8_t> Expression,
                            unsigned AddressSize, uint64_t InfoOffset,
                            bool IsVariable, uint32_t AddrBase = 0,
                            const IndexTable *Addresses = nullptr,
                            const UnitInfo *Unit = nullptr) {
  if (Unit && Unit->Version >= 5)
    forEachTypeOperand(
        Expression, AddressSize, [&](size_t Offset, unsigned Size) {
          const uint8_t *Operand = Expression.data() + Offset;
          // 0 is not an entry: it stands for the type of an address.
          if (uint64_t Type = readULEB(Operand))
            writePaddedULEB(Expression.data() + Offset, Size,
                            Object.infoOutOffset(Unit->InOffset + Type) -
                                Unit->OutOffset);
        });
  bool IsDead = false;
  forEachAddressOperand(
      Expression, AddressSize, [&](size_t Offset, unsigned Size, bool IsIndex) {
        uint8_t *P = Expression.data() + Offset;
        uint64_t Address = 0;
        const AddressRange *Range = findOperand(
            Object, P, Size, IsIndex, AddressSize, AddrBase,
            InfoOffset == NoOffset ? NoOffset : InfoOffset + Offset, Address);
        IsDead |= !Range;
        if (!IsIndex) {
          writeUnsigned(P, Size,
                        Range ? Address - Range->Begin + Range->Linked : 0);
          return;
        }
        const uint8_t *Operand = P;
        uint64_t Index = readULEB(Operand);
        if (Addresses &&
            addressSlot(Object, Index, AddressSize, AddrBase) != NoOffset)
          writeIndex(dwarf::DW_FORM_addrx, P, Size, Addresses->outIndex(Index));
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
// type is in are part of its identity. An instance of a template has to
// have its arguments in its name ("vector<int>") to be shared: if it is just
// "vector", as with -gsimple-template-names, its name does not say which
// instance it is.
//
// The abstract instance of an inline function, the entry that its inlined
// copies refer to for what they have in common, is shared too:
// - With line tables only, it is a name and nothing else. All entries that
//   say the same are one, in any language.
// - Otherwise the function has to be one that the rule covers, with
//   DW_AT_external. Its entry has its linkage name. For a member function,
//   the entry has no name and refers to the declaration in the class; then
//   the declaration's linkage name is the identity.
// - An instance that has a static variable of the function in it, with the
//   variable's address, is not shared: the address is that of the object
//   file's own copy of the variable, and the binary has only one of them.
//
// A reference can also lead into a type: to the declaration of a member
// function that the object file defines or calls, or to a type declared in
// the type. And every parameter and variable of an inlined copy of a
// function refers to its counterpart in the abstract instance. The copies
// of a type or of an abstract instance are not the same byte for byte, and
// not every copy has every member: implicit members and instances of member
// templates are only where they are used, and units differ in which
// variables and blocks of a function they describe. So the members get
// identities too, made from that of the entry they are in and what the
// member's entry says, and the owner of an entry publishes those of its
// copy. A reference into another object's entry goes to the member with
// that identity there. If there is none, the object keeps its own copy of
// the entry after all, and offers the member in it to the object files
// after it: the first object file that needs a member that the owner lacks
// has the spare copy, and the others refer to that.
//
// So that the result does not depend on the order in which threads get to
// the object files, this takes passes over all of them: scan() finds every
// object's nodes and what it would keep without uniquing, recordOwners()
// finds the owners among those, findOwnedMembers() and recordMembers()
// publish the members of what the owners have, markShared() decides what
// is kept with those and finds the members that the owners lack,
// recordSpareMembers() publishes them, and resolve() decides once more for
// the object files that can use another one's spare copy.
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

/// Whether \p Tag is that of a template argument.
bool isTemplateParameter(uint16_t Tag) {
  return Tag == dwarf::DW_TAG_template_type_parameter ||
         Tag == dwarf::DW_TAG_template_value_parameter ||
         Tag == dwarf::DW_TAG_GNU_template_template_param ||
         Tag == dwarf::DW_TAG_GNU_template_parameter_pack;
}

/// For every identity, its owner: the first of the object files that keep a
/// node with it, and the node's offset there. Filled concurrently.
class OwnerTable {
public:
  /// \p NumNodes is the most identities it can get. There are usually many
  /// nodes with one identity, so this leaves more room than it looks like.
  explicit OwnerTable(size_t NumNodes)
      : Keys(llvm::bit_ceil(std::max<size_t>(NumNodes + NumNodes / 4, 16))),
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

/// An element of Scanner::Refs is at first an offset in __debug_info that an
/// entry refers to, in the low 30 bits.
///
/// Set if the reference can be redirected to another object's entry: it then
/// has to become a DW_FORM_ref_addr of four bytes.
constexpr uint32_t RedirectableRef = uint32_t(1) << 31;
/// Set when the reference has been followed and is now the index of the node
/// that the offset is in, or NoNode: marking what is kept happens twice, and
/// finding the node of an offset is most of its work. A reference into a
/// shared node stays an offset: where it leads depends on the member that
/// the offset is.
constexpr uint32_t RefIsNode = uint32_t(1) << 30;
/// With RefIsNode: set if the offset was that of the node's own entry.
constexpr uint32_t RefToOwnEntry = uint32_t(1) << 29;
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
  /// Whether another object file owns the entry. Known when what is kept is
  /// decided for the second time.
  bool IsForeign = false;
  /// Whether Redirects says that references to the entry go to its owner.
  bool IsRedirected = false;
  /// What makes this entry the same as entries of other object files, or 0.
  uint64_t Identity = 0;
  /// Whether one copy of the entry is kept for all object files that have
  /// it. Not the case for a namespace: every unit keeps an entry for it
  /// around what it keeps of the namespace, as with dsymutil. Its identity
  /// is only a part of those of the entries in it.
  bool IsShared = false;
};

/// What an entry says about its names.
struct EntryNames {
  /// Offsets in the object's strings, or NoString.
  uint32_t Name = NoString;
  uint32_t LinkageName = NoString;
  /// The entry that this one specifies or is an instance of, which has the
  /// names that this one lacks: its offset in __debug_info, or 0.
  uint32_t Origin = 0;
  /// Whether it has DW_AT_external: other units can have the same thing.
  bool IsExternal = false;
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

/// An entry in a shared node that other entries can refer to.
struct Member {
  /// What makes it the same as a member of the other copies of the node, or
  /// 0.
  uint64_t Identity;
  uint32_t Offset;
  /// Whether Redirects says that references to it go to another object
  /// file's copy.
  bool IsRedirected = false;
  /// Whether it is in SpareMembers: the node's owner has no such member.
  bool IsOffered = false;
};

/// A member of a node that this object file does not own, which the owner's
/// copy of the node lacks and something here refers to.
struct SpareMember {
  uint64_t Identity;
  uint32_t Offset;
  uint32_t Node;
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
  /// Finds the members of the nodes that this object file owns, and returns
  /// their number.
  size_t findOwnedMembers(const OwnerTable &Owners);
  void recordMembers(OwnerTable &Members);
  /// Decides what is kept, with references to what other object files own
  /// going there. Returns the number of members that it found missing
  /// there.
  size_t markShared(const OwnerTable &Owners, const OwnerTable &Members);
  void recordSpareMembers(OwnerTable &Spares) const;
  /// Decides what is kept in the end, and prepares the object file's
  /// contributions to the output as far as they do not depend on other
  /// object files.
  void resolve(const OwnerTable &Spares, bool WithNameTags);

private:
  const char *scanUnit(ScannedUnit &Unit);
  void readBases(ScannedUnit &Unit);
  uint64_t mixAttribute(uint64_t Identity, AttributeSpec Spec,
                        const uint8_t *Value, uint64_t Size,
                        const UnitHeader &Header);
  uint64_t stringHash(uint32_t Offset);
  int32_t nodeAt(uint32_t Offset) const;
  void markKept();
  void keep(int32_t Index, bool Whole);
  void keepReferenced();
  const ScannedUnit *unitAt(uint32_t Offset) const;
  void useAbbreviations(uint32_t Offset);
  std::vector<Member> membersOf(uint32_t NodeIndex);
  bool redirectMember(uint32_t NodeIndex, uint32_t Offset);
  void addRanges(ScannedUnit &Unit);
  void collectFromKeptEntries(const UnitInfo &Unit);
  bool addRangeList(const UnitInfo &Unit, uint64_t Offset,
                    const AddressRange *Function, SmallVectorImpl<char> &Out);
  void addLocationList(const UnitInfo &Unit, uint64_t Offset,
                       const AddressRange *Function, ListTable *Table);
  void addRangeListEntry(const UnitInfo &Unit, SmallVectorImpl<char> &Out,
                         uint64_t Begin, uint64_t End);
  void endRangeList(const UnitInfo &Unit, SmallVectorImpl<char> &Out);
  uint32_t addString(StringRef Str);
  void noteName(EntryNames &Names, AttributeSpec Spec, const uint8_t *Value,
                uint64_t Size, uint32_t UnitOffset, uint32_t StrOffsetsBase);
  EntryNames readNames(uint32_t Offset);
  void completeNames(EntryNames &Names);
  void useExtraString(uint32_t Name);
  void addName(std::vector<AccelName> &Table, uint32_t Name, uint32_t Entry,
               uint16_t Tag);
  void addCodeNames(EntryNames Names, uint16_t Tag, uint32_t Entry);

  ObjectFile &Object;
  /// The index of Object in the debug map.
  uint32_t Index;
  /// The table of the unit that is being worked on, and where it is in
  /// __debug_abbrev.
  AbbreviationTable Abbreviations;
  uint32_t AbbrevOffset = UINT32_MAX;
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
  size_t NumIdentities = 0;
  /// Kept nodes whose references still have to be followed.
  std::vector<uint32_t> Worklist;
  /// The members of the shared nodes that this object file owns.
  std::vector<Member> OwnedMembers;
  /// The members of the nodes of other owners that a reference led into, by
  /// the node's index.
  DenseMap<uint32_t, std::vector<Member>> ForeignMembers;
  /// Those of them that the owners lack.
  std::vector<SpareMember> SpareMembers;
  /// What other object files have, as far as it is known: the owners of
  /// nodes, the members in their copies, and the spare copies of the members
  /// that those lack.
  const OwnerTable *Owners = nullptr;
  const OwnerTable *Members = nullptr;
  const OwnerTable *Spares = nullptr;
  /// For each attribute of Abbreviations: whether RefAddrForms has it.
  BitVector IsRefAddrForm;
  /// The linked address ranges of the live functions, in file order.
  std::vector<std::pair<uint64_t, uint64_t>> FunctionRanges;
  bool HasLiveVariable = false;
  /// Whether the tags of the entries that have names are wanted.
  bool WithNameTags = false;
};
} // namespace

/// Finds the tables of the DWARF 5 unit \p Unit that its entries index
/// into. The unit's own entry says where they are, in attributes that follow
/// the ones that need them. Also finds the entry's DW_AT_low_pc.
void Scanner::readBases(ScannedUnit &Unit) {
  UnitHeader &Header = Unit.Header;
  const uint8_t *P =
      Object.Info.bytes_begin() + Unit.Offset + Header.HeaderSize;
  const uint8_t *End = Object.Info.bytes_begin() + Unit.Offset + Header.Size;
  // The scan reports what is wrong with the entry.
  const Abbreviation *Abbrev =
      P < End ? Abbreviations.find(readULEB(P)) : nullptr;
  if (!Abbrev)
    return;
  for (AttributeSpec Spec : Abbreviations.attributes(*Abbrev)) {
    uint64_t Size = formSize(Spec.Form, P, Header.Version, Header.AddressSize);
    if (Size > uint64_t(End - P))
      return;
    if (Spec.Form == dwarf::DW_FORM_sec_offset) {
      if (Spec.Attribute == dwarf::DW_AT_str_offsets_base)
        Header.StrOffsetsBase = read32le(P);
      else if (Spec.Attribute == dwarf::DW_AT_addr_base)
        Header.AddrBase = read32le(P);
      else if (Spec.Attribute == dwarf::DW_AT_rnglists_base)
        Header.RngListsBase = read32le(P);
      else if (Spec.Attribute == dwarf::DW_AT_loclists_base)
        Header.LocListsBase = read32le(P);
    } else if (Spec.Attribute == dwarf::DW_AT_low_pc &&
               isAddressIndex(Spec.Form)) {
      Header.LowPcOffset = P - Object.Info.bytes_begin();
      Header.LowPcFormOffset = Spec.FormOffset;
      Header.LowPcSize = Size;
    }
    P += Size;
  }
}

const char *Scanner::scanUnit(ScannedUnit &Unit) {
  if (!Abbreviations.parse(Object.Abbrev, Unit.Header.AbbrevOffset))
    return "malformed abbreviation table";
  AbbrevOffset = Unit.Header.AbbrevOffset;
  if (Unit.Header.Version >= 5)
    readBases(Unit);
  const UnitHeader &Header = Unit.Header;

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
  // Whether Current is a type whose name has no template arguments in it.
  bool CurrentHasBareName = false;
  bool CurrentIsAbstractFunction = false;
  bool CurrentIsFunction = false;

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
    if (Current >= 0 && Depth == CurrentDepth + 1 && CurrentHasBareName &&
        isTemplateParameter(Abbrev->Tag)) {
      Nodes[Current].Identity = 0;
      Nodes[Current].IsShared = false;
    }
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
      N.IsRoot = Object.IsModule || Abbrev->Tag == dwarf::DW_TAG_base_type ||
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
    // The abstract instance of an inline function. Whether it has an
    // identity shows when its attributes are read.
    bool IsAbstractFunction = StartsNode && Abbrev->HasInline &&
                              Abbrev->Tag == dwarf::DW_TAG_subprogram;
    bool HasReference = false;
    bool IsExternal = false;
    uint32_t Specification = 0;
    if (StartsNode &&
        (IsAbstractFunction || hasIdentity(Abbrev->Tag, Unit.HasODR))) {
      int32_t Parent = Nodes.back().Parent;
      Identity = Parent < 0 ? RootIdentity : Nodes[Parent].Identity;
      if (Identity)
        Identity = mixIdentity(Identity, Abbrev->Tag) | 1;
      CurrentHasBareName = false;
    }
    if (StartsNode)
      CurrentIsAbstractFunction = IsAbstractFunction;
    if (StartsNode)
      CurrentIsFunction = Abbrev->Tag == dwarf::DW_TAG_subprogram;
    // The symbol of the function that this entry describes, if it is live.
    AddressRange *LiveFunction = nullptr;

    for (AttributeSpec Spec : Abbreviations.attributes(*Abbrev)) {
      uint64_t Size =
          formSize(Spec.Form, P, Header.Version, Header.AddressSize);
      if (Size == UnsupportedForm)
        return "unsupported attribute form";
      if (Size > uint64_t(End - P))
        return "attribute extends past the end of its unit";
      if (isStringIndex(Spec.Form) &&
          stringOffset(Object, Spec.Form, P, Header.StrOffsetsBase) == NoString)
        return "index of a string is outside of __debug_str_offs";
      if (isAddressIndex(Spec.Form) &&
          addressSlot(Object, readIndex(Spec.Form, P), Header.AddressSize,
                      Header.AddrBase) == NoOffset)
        return "index of an address is outside of __debug_addr";
      if (LLVM_UNLIKELY(isListIndex(Spec.Form)) &&
          (Spec.Form == dwarf::DW_FORM_rnglistx
               ? indexedList(Object.RngLists, Header.RngListsBase,
                             readIndex(Spec.Form, P))
               : indexedList(Object.LocLists, Header.LocListsBase,
                             readIndex(Spec.Form, P))) == NoOffset)
        return "index of a list is outside of its table";
      if (Identity && !isReference(Spec.Form) &&
          Spec.Attribute != dwarf::DW_AT_decl_file) {
        Identity = mixAttribute(Identity, Spec, P, Size, Header);
        if (Spec.Attribute == dwarf::DW_AT_name) {
          NeedsName = false;
          uint32_t Name =
              stringOffset(Object, Spec.Form, P, Header.StrOffsetsBase);
          CurrentHasBareName =
              !IsAbstractFunction &&
              !(Name != NoString && Object.string(Name).contains('<'));
        }
      }
      if (isReference(Spec.Form)) {
        // References made by the unit's own entry or by containers are not
        // followed. They do not occur in practice.
        if (Current >= 0 && Spec.Attribute != dwarf::DW_AT_sibling) {
          uint64_t Target = readReference(P, Size);
          if (Spec.Form != dwarf::DW_FORM_ref_addr)
            Target += Unit.Offset;
          // In DWARF 2 a DW_FORM_ref_addr has the size of an address.
          bool IsRedirectable = Size == 4 && Header.Version >= 3 &&
                                (Spec.Form == dwarf::DW_FORM_ref4 ||
                                 Spec.Form == dwarf::DW_FORM_ref_addr);
          if (Target < Object.Info.size())
            Refs.push_back(Target | (IsRedirectable ? RedirectableRef : 0));
          HasReference = true;
          if (Spec.Attribute == dwarf::DW_AT_specification &&
              Target < Object.Info.size())
            Specification = Target;
        }
      } else {
        switch (Spec.Attribute) {
        case dwarf::DW_AT_low_pc:
          if (!isAddress(Spec.Form))
            break;
          if (IsUnitEntry) {
            Unit.LowPc = addressValue(Object, Spec.Form, P, Header.AddressSize,
                                      Header.AddrBase);
            Unit.HasLowPc = true;
          } else if (IsFunction && Current >= 0) {
            if (StartsCurrent)
              Nodes[Current].HasCode = true;
            LiveFunction = Object.findStart(addressValue(
                Object, Spec.Form, P, Header.AddressSize, Header.AddrBase));
            if (LiveFunction)
              Nodes[Current].IsRoot = true;
          }
          break;
        case dwarf::DW_AT_high_pc: {
          if (!LiveFunction)
            break;
          // An address, or a length. The symbol can be longer than the
          // function, by the padding that follows it; the debug info should
          // not cover that. The function can also be longer than its
          // symbol: see AddressRange::Limit.
          uint64_t FunctionEnd =
              isAddress(Spec.Form)
                  ? addressValue(Object, Spec.Form, P, Header.AddressSize,
                                 Header.AddrBase)
                  : LiveFunction->Begin + readUnsigned(Spec.Form, P, Size);
          if (FunctionEnd > LiveFunction->Begin)
            LiveFunction->End = std::min(FunctionEnd, LiveFunction->Limit);
          break;
        }
        case dwarf::DW_AT_const_value:
          // Constants outside of functions are always kept, as in dsymutil.
          // One in a type keeps the type: in DWARF 5 that is what a static
          // member of a class is.
          if (Current >= 0 && !CurrentIsFunction &&
              (Abbrev->Tag == dwarf::DW_TAG_variable ||
               Abbrev->Tag == dwarf::DW_TAG_constant))
            Nodes[Current].IsRoot = true;
          break;
        case dwarf::DW_AT_location: {
          // A static variable of an inline function is described in the
          // function's abstract instance, with its address. Only one object
          // file's variable is in the binary, and so its instance is not
          // the same as the others'.
          bool IsInSharedFunction = Current >= 0 && !StartsCurrent &&
                                    CurrentIsAbstractFunction &&
                                    Nodes[Current].IsShared;
          if ((!IsVariable && !IsInSharedFunction) ||
              !isExpression(Spec, Header.Version))
            break;
          ArrayRef<uint8_t> Location = blockContents(Spec.Form, P);
          forEachAddressOperand(
              Location, Header.AddressSize,
              [&](size_t Offset, unsigned Size, bool IsIndex) {
                if (IsInSharedFunction) {
                  Nodes[Current].Identity = 0;
                  Nodes[Current].IsShared = false;
                  return;
                }
                // A variable is live if it is where a symbol of the debug
                // map is.
                uint64_t Address;
                if (findOperand(Object, Location.data() + Offset, Size, IsIndex,
                                Header.AddressSize, Header.AddrBase,
                                Location.data() + Offset - Base, Address))
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
        case dwarf::DW_AT_external:
          IsExternal = readFlag(Spec.Form, P);
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
    if (IsAbstractFunction && Identity) {
      NeedsName = false;
      if (!Abbrev->HasChildren && !HasReference) {
        // The entry is all there is to it.
      } else if (!Unit.HasODR) {
        Identity = 0;
      } else if (Specification) {
        EntryNames Declaration = readNames(Specification);
        Identity =
            Declaration.IsExternal && Declaration.LinkageName != NoString
                ? mixIdentity(Identity, stringHash(Declaration.LinkageName)) | 1
                : 0;
      } else if (!IsExternal) {
        Identity = 0;
      }
    }
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

/// Adds an attribute to the identity \p Identity of its entry, which is in
/// the unit with the header \p Header.
uint64_t Scanner::mixAttribute(uint64_t Identity, AttributeSpec Spec,
                               const uint8_t *Value, uint64_t Size,
                               const UnitHeader &Header) {
  // The index of a string is another one in every object file.
  uint32_t String =
      stringOffset(Object, Spec.Form, Value, Header.StrOffsetsBase);
  uint64_t Hash = String != NoString ? stringHash(String)
                                     : xxh3_64bits(ArrayRef(Value, Size));
  // 0 means that there is no identity.
  return mixIdentity(
             mixIdentity(Identity, uint64_t(Spec.Attribute) << 16 | Spec.Form),
             Hash) |
         1;
}

uint64_t Scanner::stringHash(uint32_t Offset) {
  return xxh3_64bits(Object.string(Offset));
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
void Scanner::keepReferenced() {
  while (!Worklist.empty()) {
    const Node &N = Nodes[Worklist.back()];
    Worklist.pop_back();
    uint32_t EndRef = N.IsKeptWhole ? N.EndRef : N.EndOwnRef;
    for (uint32_t I = N.FirstRef; I != EndRef; ++I) {
      uint32_t Ref = Refs[I];
      uint32_t Target = Ref & NoNode;
      bool IsToOwnEntry = Ref & RefToOwnEntry;
      uint32_t Offset = 0;
      if (!(Ref & RefIsNode)) {
        Offset = Ref & (RefIsNode - 1);
        int32_t Found = nodeAt(Offset);
        Target = Found < 0 ? NoNode : Found;
        IsToOwnEntry = Found >= 0 && Offset == Nodes[Found].Begin;
        if (Found < 0 || IsToOwnEntry || !Nodes[Found].IsShared)
          Refs[I] = (Ref & RedirectableRef) | RefIsNode | Target |
                    (IsToOwnEntry ? RefToOwnEntry : 0);
      }
      if (Target == NoNode)
        continue;
      Node &T = Nodes[Target];
      if (T.IsForeign && (Ref & RedirectableRef)) {
        // Another object file owns the entry. The reference goes to its
        // copy, if that has what the reference is to. Redirects gets one
        // record for all references to the same thing.
        if (IsToOwnEntry) {
          if (!T.IsRedirected) {
            uint64_t Owner = Owners->lookup(T.Identity);
            Object.Redirects.push_back({T.Begin, OwnerTable::objectOf(Owner),
                                        OwnerTable::offsetOf(Owner)});
            T.IsRedirected = true;
          }
          continue;
        }
        if (redirectMember(Target, Offset))
          continue;
      }
      keep(Target, /*Whole=*/!IsToOwnEntry);
    }
  }
}

/// The unit that \p Offset of __debug_info is in, or null.
const ScannedUnit *Scanner::unitAt(uint32_t Offset) const {
  auto It = llvm::upper_bound(
      Units, Offset,
      [](uint32_t Offset, const ScannedUnit &U) { return Offset < U.Offset; });
  return It == Units.begin() ? nullptr : &*std::prev(It);
}

/// Makes Abbreviations the table at \p Offset of __debug_abbrev, which the
/// scan has parsed before, successfully.
void Scanner::useAbbreviations(uint32_t Offset) {
  if (AbbrevOffset == Offset)
    return;
  Abbreviations.parse(Object.Abbrev, Offset);
  AbbrevOffset = Offset;
}

/// Finds the members of the shared node \p NodeIndex: the entries in it that
/// entries outside of it can refer to.
///
/// In a type, those are the children of the type's entry and, if one of them
/// is a type, its children, and so on. The identity of a member is made like
/// that of a node, from the identity of the type it is in, its tag and its
/// attributes. For a function, the tags of its children and of the types of
/// its parameters are added: that tells the implicit copy constructor from
/// the implicit move constructor, which have nothing else to tell them
/// apart.
///
/// In the abstract instance of a function, they are its parameters and
/// variables, and the types declared in it with their members. Variables and
/// types can be in blocks, and units differ in the blocks that they describe
/// of a function: the identity of what is in a block is made as if it were
/// not in one. A parameter's position is a part of its identity, since
/// parameters need no names.
///
/// If two members of a node still have the same identity, they get none.
std::vector<Member> Scanner::membersOf(uint32_t NodeIndex) {
  const Node &N = Nodes[NodeIndex];
  const ScannedUnit &Unit = *unitAt(N.Begin);
  useAbbreviations(Unit.Header.AbbrevOffset);
  const uint8_t *Base = Object.Info.bytes_begin();
  // The tag of the entry at Offset, which can be in another unit of the
  // object file: with LTO, a type often is.
  auto TagAt = [&](uint64_t Offset) -> uint64_t {
    const ScannedUnit *Other = unitAt(Offset);
    if (!Other || Offset < Other->Offset + Other->Header.HeaderSize ||
        Offset >= Other->Offset + Other->Header.Size)
      return 0;
    const AbbreviationTable *Table = &Abbreviations;
    if (Other->Header.AbbrevOffset != AbbrevOffset) {
      // The scan has parsed this table before, successfully.
      if (Other->Header.AbbrevOffset != OriginAbbrevOffset)
        OriginAbbreviations.parse(Object.Abbrev, Other->Header.AbbrevOffset);
      OriginAbbrevOffset = Other->Header.AbbrevOffset;
      Table = &OriginAbbreviations;
    }
    const uint8_t *P = Base + Offset;
    const Abbreviation *Abbrev = Table->find(readULEB(P));
    return Abbrev ? Abbrev->Tag : 0;
  };

  // The entries that the current one is in, starting with the node's.
  struct Scope {
    /// The identity that it gives to the members in it, or 0.
    uint64_t Identity;
    /// Whether it is the abstract instance of a function or a block in it.
    bool IsBody;
    /// If the scope is a function that is a member of a type: which one.
    int32_t Function;
    /// If the scope is a type that is a member: which one, and whether its
    /// name has template arguments in it.
    int32_t Type;
    bool HasArguments;
  };
  SmallVector<Scope, 8> Scopes;
  Scopes.push_back(
      {N.Identity, TagAt(N.Begin) == dwarf::DW_TAG_subprogram, -1, -1, false});
  unsigned NumParameters = 0;
  std::vector<Member> Result;
  const uint8_t *P = Base + N.OwnEnd;
  const uint8_t *End = Base + N.End;
  while (P < End && !Scopes.empty()) {
    uint32_t Offset = P - Base;
    uint64_t Code = readULEB(P);
    if (Code == 0) {
      Scopes.pop_back();
      continue;
    }
    const Abbreviation &Abbrev = *Abbreviations.find(Code);
    // A type in the node is not shared either if its name does not say
    // which instance of a template it is.
    if (Scopes.back().Type >= 0 && !Scopes.back().HasArguments &&
        isTemplateParameter(Abbrev.Tag)) {
      Result[Scopes.back().Type].Identity = 0;
      Scopes.back().Identity = 0;
    }
    Scope Parent = Scopes.back();
    uint64_t Identity =
        Parent.Identity ? mixIdentity(Parent.Identity, Abbrev.Tag) | 1 : 0;
    if (Identity && Parent.IsBody &&
        Abbrev.Tag == dwarf::DW_TAG_formal_parameter)
      Identity = mixIdentity(Identity, ++NumParameters) | 1;
    bool HasArguments = false;
    if (Parent.Function >= 0)
      Result[Parent.Function].Identity =
          mixIdentity(Result[Parent.Function].Identity, Abbrev.Tag) | 1;
    for (AttributeSpec Spec : Abbreviations.attributes(Abbrev)) {
      uint64_t Size =
          formSize(Spec.Form, P, Unit.Header.Version, Unit.Header.AddressSize);
      if (Identity && !isReference(Spec.Form) &&
          Spec.Attribute != dwarf::DW_AT_decl_file)
        Identity = mixAttribute(Identity, Spec, P, Size, Unit.Header);
      if (Spec.Attribute == dwarf::DW_AT_name) {
        uint32_t Name =
            stringOffset(Object, Spec.Form, P, Unit.Header.StrOffsetsBase);
        HasArguments = Name != NoString && Object.string(Name).contains('<');
      }
      // The type is in this unit, or in another one of the object file.
      if (Parent.Function >= 0 && Spec.Attribute == dwarf::DW_AT_type &&
          Size == 4 &&
          (Spec.Form == dwarf::DW_FORM_ref4 ||
           Spec.Form == dwarf::DW_FORM_ref_addr))
        Result[Parent.Function].Identity =
            mixIdentity(
                Result[Parent.Function].Identity,
                TagAt(read32le(P) +
                      (Spec.Form == dwarf::DW_FORM_ref4 ? Unit.Offset : 0))) |
            1;
      P += Size;
    }
    // Nothing refers to a block of an abstract instance.
    bool IsBlock = Parent.IsBody && Abbrev.Tag == dwarf::DW_TAG_lexical_block;
    if (Identity && !IsBlock)
      Result.push_back({Identity, Offset});
    if (!Abbrev.HasChildren)
      continue;
    if (IsBlock) {
      Scopes.push_back({Parent.Identity, /*IsBody=*/true, -1, -1, false});
      continue;
    }
    bool IsType = Abbrev.Tag == dwarf::DW_TAG_class_type ||
                  Abbrev.Tag == dwarf::DW_TAG_structure_type ||
                  Abbrev.Tag == dwarf::DW_TAG_union_type ||
                  Abbrev.Tag == dwarf::DW_TAG_enumeration_type;
    bool IsFunction = Identity && Abbrev.Tag == dwarf::DW_TAG_subprogram;
    int32_t Record = Identity ? int32_t(Result.size() - 1) : -1;
    Scopes.push_back({IsType ? Identity : 0, /*IsBody=*/false,
                      IsFunction ? Record : -1, IsType ? Record : -1,
                      HasArguments});
  }

  std::vector<uint64_t> Identities;
  for (const Member &M : Result)
    Identities.push_back(M.Identity);
  llvm::sort(Identities);
  for (Member &M : Result) {
    auto Same =
        std::equal_range(Identities.begin(), Identities.end(), M.Identity);
    if (Same.second - Same.first > 1)
      M.Identity = 0;
  }
  return Result;
}

/// Sends references to the entry at \p Offset in the node \p NodeIndex,
/// which another object file owns, to the owner's copy, if that has such a
/// member. Returns whether it has.
bool Scanner::redirectMember(uint32_t NodeIndex, uint32_t Offset) {
  auto [It, Inserted] = ForeignMembers.try_emplace(NodeIndex);
  if (Inserted)
    It->second = membersOf(NodeIndex);
  auto Found = llvm::lower_bound(
      It->second, Offset,
      [](const Member &M, uint32_t Offset) { return M.Offset < Offset; });
  if (Found == It->second.end() || Found->Offset != Offset)
    return false;
  if (Found->IsRedirected || !Found->Identity)
    return Found->IsRedirected;
  uint64_t Owner = Members->lookup(Found->Identity);
  if (!Owner) {
    if (!Spares) {
      // The owner's copy lacks the member. Offer this one to the others.
      if (!Found->IsOffered)
        SpareMembers.push_back({Found->Identity, Offset, NodeIndex});
      Found->IsOffered = true;
      return false;
    }
    // The spare copy is an earlier object file's, or one of this one's: no
    // references within an object file, see markKept().
    Owner = Spares->lookup(Found->Identity);
    if (!Owner || OwnerTable::objectOf(Owner) == Index) {
      // Do not ask again.
      Found->Identity = 0;
      return false;
    }
  }
  Object.Redirects.push_back(
      {Offset, OwnerTable::objectOf(Owner), OwnerTable::offsetOf(Owner)});
  Found->IsRedirected = true;
  return true;
}

size_t Scanner::findOwnedMembers(const OwnerTable &Owners) {
  for (uint32_t I = 0; I != Nodes.size(); ++I) {
    const Node &N = Nodes[I];
    if (!N.IsKept || !N.IsShared || N.IsContainer || N.OwnEnd == N.End ||
        Owners.lookup(N.Identity) != OwnerTable::owner(Index, N.Begin))
      continue;
    for (const Member &M : membersOf(I))
      if (M.Identity)
        OwnedMembers.push_back(M);
  }
  return OwnedMembers.size();
}

void Scanner::recordMembers(OwnerTable &Members) {
  for (const Member &M : OwnedMembers)
    Members.record(M.Identity, Index, M.Offset);
  OwnedMembers = std::vector<Member>();
}

/// Decides what is kept: the roots, with \p Owners the nodes that this
/// object file owns, and what those refer to.
void Scanner::markKept() {
  for (Node &N : Nodes)
    N.IsKept = N.IsKeptWhole = N.IsRedirected = false;
  for (uint32_t I = 0; I != Nodes.size(); ++I) {
    Node &N = Nodes[I];
    bool IsOwned = false;
    if (Owners && N.IsShared) {
      // Entries of one object file that are the same stay as they are:
      // references between the entries of an object file can go forward,
      // and not every reader copes with several of those to one entry.
      uint64_t Owner = Owners->lookup(N.Identity);
      IsOwned = Owner == OwnerTable::owner(Index, N.Begin);
      N.IsForeign = Owner && OwnerTable::objectOf(Owner) != Index;
    }
    if ((N.IsRoot && !N.IsContainer) || IsOwned)
      keep(I, /*Whole=*/true);
  }
  // The copies that other object files refer into for a member that the
  // owner's copy lacks.
  if (Spares)
    for (const SpareMember &S : SpareMembers)
      if (Spares->lookup(S.Identity) == OwnerTable::owner(Index, S.Offset))
        keep(S.Node, /*Whole=*/true);
  keepReferenced();
}

size_t Scanner::markShared(const OwnerTable &Owners,
                           const OwnerTable &Members) {
  if (Nodes.empty())
    return 0;
  this->Owners = &Owners;
  this->Members = &Members;
  markKept();
  // resolve() only looks at the members again if some are missing.
  if (SpareMembers.empty())
    ForeignMembers.clear();
  return SpareMembers.size();
}

void Scanner::recordSpareMembers(OwnerTable &Spares) const {
  for (const SpareMember &S : SpareMembers)
    Spares.record(S.Identity, Index, S.Offset);
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
  Info.StrOffsetsBase = Unit.Header.StrOffsetsBase;
  Info.AddrBase = Unit.Header.AddrBase;
  Info.RngListsBase = Unit.Header.RngListsBase;
  Info.LocListsBase = Unit.Header.LocListsBase;
  Info.LowPcOffset = Unit.Header.LowPcOffset;
  Info.LowPcFormOffset = Unit.Header.LowPcFormOffset;
  Info.LowPcSize = Unit.Header.LowPcSize;
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
  // Whether the next range cannot continue the one before.
  bool StartsRange = true;
  auto Add = [&](uint32_t Begin, uint32_t End) {
    if (Begin == End)
      return;
    if (!StartsRange && Object.KeptRanges.back().InEnd == Begin)
      Object.KeptRanges.back().InEnd = End;
    else
      Object.KeptRanges.push_back({Begin, End, Out});
    Out += End - Begin;
    StartsRange = false;
  };

  // The unit's own entry, the kept nodes, and the null entries that end the
  // children of the unit and of the kept containers.
  Add(Unit.Offset + Unit.Header.HeaderSize, Unit.OwnEnd);
  if (Info.LowPcSize) {
    // The entry is not copied as it is: see UnitInfo::LowPcOffset.
    Out += Info.AddressSize - Info.LowPcSize;
    StartsRange = true;
  }
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
    if (const char *Problem = linkLineTable(Object, Unit.LineTable))
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

static void appendAddress(SmallVectorImpl<char> &Out, uint64_t Address,
                          unsigned Size) {
  char Bytes[8];
  writeUnsigned(reinterpret_cast<uint8_t *>(Bytes), Size, Address);
  Out.append(Bytes, Bytes + Size);
}

static void appendULEB(SmallVectorImpl<char> &Out, uint64_t Value) {
  uint8_t Bytes[10];
  unsigned Size = encodeULEB128(Value, Bytes);
  Out.append(Bytes, Bytes + Size);
}

namespace {
/// Reads a range list or a location list of DWARF 5: entries that start
/// with a byte that says what they are. Addresses are those of the object
/// file.
struct ListReader {
  const ObjectFile &Object;
  const UnitInfo &Unit;
  /// The next entry, and the end of the section.
  const uint8_t *P;
  const uint8_t *End;
  bool IsLocationList;
  /// What entries with offsets are relative to.
  uint64_t Base;

  enum Kind { EndOfList, Range, DefaultLocation };
  /// Reads up to the next entry that is one of these. Whatever is malformed
  /// ends the list. A range is in \p Begin and \p RangeEnd. In a location
  /// list, an expression with its length follows at P.
  Kind next(uint64_t &Begin, uint64_t &RangeEnd);
};
} // namespace

ListReader::Kind ListReader::next(uint64_t &Begin, uint64_t &RangeEnd) {
  unsigned Size = Unit.AddressSize;
  bool IsMalformed = false;
  // Numbers are read without checking against End: they are LEB128s that a
  // few bytes at most extend past it, where the file is still mapped.
  auto Number = [&] { return P < End ? readULEB(P) : 0; };
  auto Address = [&]() -> uint64_t {
    if (uint64_t(End - P) < Size) {
      IsMalformed = true;
      return 0;
    }
    P += Size;
    return readAddress(P - Size, Size);
  };
  auto IndexedAddress = [&]() -> uint64_t {
    uint64_t Slot = addressSlot(Object, Number(), Size, Unit.AddrBase);
    IsMalformed |= Slot == NoOffset;
    return IsMalformed ? 0
                       : readAddress(Object.Addr.bytes_begin() + Slot, Size);
  };
  while (P < End) {
    // The kinds have the numbers of range lists here. Location lists have
    // one more, in the middle.
    uint8_t Kind = *P++;
    if (IsLocationList && Kind == dwarf::DW_LLE_default_location)
      return DefaultLocation;
    if (IsLocationList && Kind > dwarf::DW_LLE_default_location)
      --Kind;
    switch (Kind) {
    case dwarf::DW_RLE_base_addressx:
      Base = IndexedAddress();
      break;
    case dwarf::DW_RLE_base_address:
      Base = Address();
      break;
    case dwarf::DW_RLE_startx_endx:
      Begin = IndexedAddress();
      RangeEnd = IndexedAddress();
      return IsMalformed ? EndOfList : Range;
    case dwarf::DW_RLE_startx_length:
      Begin = IndexedAddress();
      RangeEnd = Begin + Number();
      return IsMalformed ? EndOfList : Range;
    case dwarf::DW_RLE_offset_pair:
      Begin = Base + Number();
      RangeEnd = Base + Number();
      return Range;
    case dwarf::DW_RLE_start_end:
      Begin = Address();
      RangeEnd = Address();
      return IsMalformed ? EndOfList : Range;
    case dwarf::DW_RLE_start_length:
      Begin = Address();
      RangeEnd = Begin + Number();
      return IsMalformed ? EndOfList : Range;
    default:
      return EndOfList;
    }
    if (IsMalformed)
      break;
  }
  return EndOfList;
}

/// Appends the range from \p Begin to \p End of the binary to the range
/// list at the end of \p Out, which is one of \p Unit.
void Scanner::addRangeListEntry(const UnitInfo &Unit,
                                SmallVectorImpl<char> &Out, uint64_t Begin,
                                uint64_t End) {
  if (Unit.Version >= 5) {
    // An address and a length. With that, no list depends on the unit's
    // base address.
    Out.push_back(dwarf::DW_RLE_start_length);
    appendAddress(Out, Begin, Unit.AddressSize);
    appendULEB(Out, End - Begin);
    return;
  }
  // Two addresses, relative to the unit's.
  uint64_t Base = outBaseAddress(Object, Unit);
  char Entry[16];
  if (Unit.AddressSize == 8) {
    write64le(Entry, Begin - Base);
    write64le(Entry + 8, End - Base);
  } else {
    write32le(Entry, Begin - Base);
    write32le(Entry + 4, End - Base);
  }
  Out.append(Entry, Entry + 2 * Unit.AddressSize);
}

/// Ends the range list at the end of \p Out.
void Scanner::endRangeList(const UnitInfo &Unit, SmallVectorImpl<char> &Out) {
  if (Unit.Version >= 5) {
    Out.push_back(dwarf::DW_RLE_end_of_list);
    return;
  }
  // Two zeros.
  uint64_t Base = outBaseAddress(Object, Unit);
  addRangeListEntry(Unit, Out, Base, Base);
}

/// Translates the range list at \p Offset of the section that \p Unit has
/// its range lists in, which belongs to an entry in \p Function if that is
/// not null, and appends it to \p Out. Returns whether any of its code is in
/// the binary.
bool Scanner::addRangeList(const UnitInfo &Unit, uint64_t Offset,
                           const AddressRange *Function,
                           SmallVectorImpl<char> &Out) {
  size_t ListOffset = Out.size();
  unsigned Size = Unit.AddressSize;
  // The entry to add next, unless the entry after it continues it.
  uint64_t PendingBegin = 0, PendingEnd = 0;
  auto Add = [&](uint64_t Begin, uint64_t End) {
    if (Begin >= End)
      return;
    // Ranges of code that is not in the binary are dropped.
    const AddressRange *Range = Function;
    if (!Range || Begin < Range->Begin || Begin >= Range->End)
      Range = Object.findContaining(Begin);
    if (!Range)
      return;
    uint64_t Linked = Begin - Range->Begin + Range->Linked;
    if (PendingBegin != PendingEnd && Linked == PendingEnd) {
      PendingEnd = Linked + (End - Begin);
      return;
    }
    if (PendingBegin != PendingEnd)
      addRangeListEntry(Unit, Out, PendingBegin, PendingEnd);
    PendingBegin = Linked;
    PendingEnd = Linked + (End - Begin);
  };
  if (Unit.Version >= 5) {
    StringRef Section = Object.RngLists;
    ListReader Reader{Object,
                      Unit,
                      Section.bytes_begin() +
                          std::min<uint64_t>(Offset, Section.size()),
                      Section.bytes_end(),
                      /*IsLocationList=*/false,
                      Unit.InLowPc};
    for (uint64_t Begin, End; Reader.next(Begin, End) == ListReader::Range;)
      Add(Begin, End);
  } else {
    // Pairs of addresses. A pair of zeros ends the list.
    uint64_t MaxAddress = Size == 8 ? UINT64_MAX : UINT32_MAX;
    uint64_t Base = Unit.InLowPc;
    StringRef Section = Object.Ranges;
    for (; Offset <= Section.size() && Section.size() - Offset >= 2 * Size;
         Offset += 2 * Size) {
      uint64_t Begin = readAddress(Section.bytes_begin() + Offset, Size);
      uint64_t End = readAddress(Section.bytes_begin() + Offset + Size, Size);
      if (Begin == 0 && End == 0)
        break;
      if (Begin == MaxAddress)
        Base = End;
      else if (Begin < End)
        Add(Begin + Base, End + Base);
    }
  }
  if (PendingBegin != PendingEnd)
    addRangeListEntry(Unit, Out, PendingBegin, PendingEnd);
  bool HasCode = Out.size() != ListOffset;
  endRangeList(Unit, Out);
  return HasCode;
}

/// Adds \p Str to the object's strings and returns its offset there.
uint32_t Scanner::addString(StringRef Str) {
  uint32_t Offset = Object.extraStringsOffset() + Object.ExtraStrings.size();
  // Str can be a part of one of the strings that were added before, which
  // move when there is no room for another one.
  size_t Size = Object.ExtraStrings.size();
  bool IsExtraString = Str.begin() >= Object.ExtraStrings.begin() &&
                       Str.begin() < Object.ExtraStrings.end();
  size_t Begin = IsExtraString ? Str.begin() - Object.ExtraStrings.begin() : 0;
  Object.ExtraStrings.resize_for_overwrite(Size + Str.size() + 1);
  memcpy(Object.ExtraStrings.data() + Size,
         IsExtraString ? Object.ExtraStrings.data() + Begin : Str.data(),
         Str.size());
  Object.ExtraStrings.back() = '\0';
  return Offset;
}

/// Takes from an attribute of an entry what it says about the entry's names.
/// \p UnitOffset is where the entry's unit is in __debug_info, and
/// \p StrOffsetsBase where the unit's table of strings is.
void Scanner::noteName(EntryNames &Names, AttributeSpec Spec,
                       const uint8_t *Value, uint64_t Size, uint32_t UnitOffset,
                       uint32_t StrOffsetsBase) {
  switch (Spec.Attribute) {
  case dwarf::DW_AT_name:
  case dwarf::DW_AT_linkage_name:
  case dwarf::DW_AT_MIPS_linkage_name: {
    uint32_t Offset = stringOffset(Object, Spec.Form, Value, StrOffsetsBase);
    if (Spec.Form == dwarf::DW_FORM_string)
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
  case dwarf::DW_AT_external:
    Names.IsExternal = readFlag(Spec.Form, Value);
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
    noteName(Names, Spec, P, Size, Unit.Offset, Unit.Header.StrOffsetsBase);
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
        Object.Strings.Offsets.push_back(Name);
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
    Object.Strings.Offsets.push_back(Name);
}

/// Adds to \p Table that the entry at \p Entry of the object's contribution
/// to __debug_info, which has the tag \p Tag, is found by the name \p Name.
void Scanner::addName(std::vector<AccelName> &Table, uint32_t Name,
                      uint32_t Entry, uint16_t Tag) {
  Table.push_back({Name, Entry});
  useExtraString(Name);
  if (!WithNameTags)
    return;
  if (&Table == &Object.Names)
    Object.NameTags.push_back(Tag);
  else if (&Table == &Object.Namespaces)
    Object.NamespaceTags.push_back(Tag);
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
    addName(Object.Names, Names.LinkageName, Entry, Tag);
  if (Name.empty())
    return;
  // "f<int>" is also found as "f".
  if (HasLinkageName && Tag != dwarf::DW_TAG_inlined_subroutine)
    if (std::optional<StringRef> Stripped = StripTemplateParameters(Name))
      addName(Object.Names, addString(*Stripped), Entry, Tag);
  addName(Object.Names, Names.Name, Entry, Tag);
  // An Objective-C method "-[Class(Category) selector]" is also found by its
  // selector and without the category, and its class leads to it.
  if (std::optional<ObjCSelectorNames> ObjC = getObjCNamesIfSelector(Name)) {
    addName(Object.Names, addString(ObjC->Selector), Entry, Tag);
    addName(Object.ObjCClasses, addString(ObjC->ClassName), Entry, Tag);
    if (ObjC->ClassNameNoCategory)
      addName(Object.ObjCClasses, addString(*ObjC->ClassNameNoCategory), Entry,
              Tag);
    if (ObjC->MethodNameNoCategory)
      addName(Object.Names, addString(*ObjC->MethodNameNoCategory), Entry, Tag);
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

/// Translates the location list at \p Offset of the section that \p Unit
/// has its location lists in, which belongs to an entry in \p Function, and
/// appends it to the lists of \p Table, or to those of __debug_loc if
/// \p Table is null: then the unit is older than DWARF 5. If \p Function is
/// null, the entry is not in code of the binary, and its list is empty.
void Scanner::addLocationList(const UnitInfo &Unit, uint64_t Offset,
                              const AddressRange *Function, ListTable *Table) {
  unsigned Size = Unit.AddressSize;
  if (Table) {
    SmallVectorImpl<char> &Out = Table->Lists;
    StringRef Section = Object.LocLists;
    ListReader Reader{Object,
                      Unit,
                      Section.bytes_begin() +
                          std::min<uint64_t>(Offset, Section.size()),
                      Section.bytes_end(),
                      /*IsLocationList=*/true,
                      Unit.InLowPc};
    IndexTable *Addresses =
        Unit.AddrBase
            ? &ObjectFile::tableAt(Object.AddrTables, Unit.AddrBase, Size)
            : nullptr;
    uint64_t Begin, End;
    for (ListReader::Kind Kind; Function && (Kind = Reader.next(Begin, End)) !=
                                                ListReader::EndOfList;) {
      // An expression follows, with its length in front.
      uint64_t Length = Reader.P < Reader.End ? readULEB(Reader.P) : 0;
      if (Length > uint64_t(Reader.End - Reader.P))
        break;
      if (Kind == ListReader::DefaultLocation) {
        Out.push_back(dwarf::DW_LLE_default_location);
      } else {
        // Like dsymutil, move the entry with the function, whatever it
        // covers. An address and a length: see addRangeListEntry().
        Out.push_back(dwarf::DW_LLE_start_length);
        appendAddress(Out, Begin + Function->Linked - Function->Begin, Size);
        appendULEB(Out, End - Begin);
      }
      appendULEB(Out, Length);
      size_t ExpressionOffset = Out.size();
      Out.append(Reader.P, Reader.P + Length);
      Reader.P += Length;
      MutableArrayRef<uint8_t> Expression(
          reinterpret_cast<uint8_t *>(Out.data()) + ExpressionOffset, Length);
      patchExpression(Object, Expression, Size, NoOffset, /*IsVariable=*/false,
                      Unit.AddrBase, /*Addresses=*/nullptr, &Unit);
      // The indexes of addresses change when it is known which ones are
      // used.
      if (Addresses)
        forEachAddressOperand(
            Expression, Size, [&](size_t Offset, unsigned, bool IsIndex) {
              const uint8_t *Operand = Expression.data() + Offset;
              uint64_t Index = readULEB(Operand);
              if (!IsIndex ||
                  addressSlot(Object, Index, Size, Unit.AddrBase) == NoOffset)
                return;
              Addresses->use(Index);
              Table->AddressIndexes.emplace_back(ExpressionOffset + Offset,
                                                 Unit.AddrBase);
            });
    }
    Out.push_back(dwarf::DW_LLE_end_of_list);
    return;
  }

  SmallVectorImpl<char> &Out = Object.LocData;
  uint64_t MaxAddress = Size == 8 ? UINT64_MAX : UINT32_MAX;
  uint64_t Base = Unit.InLowPc;
  StringRef Section = Object.Loc;
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
    appendAddress(Out, Begin + Moved, Size);
    appendAddress(Out, End + Moved, Size);
    appendAddress(Out, Length, 2);
    Out.append(Section.begin() + Offset, Section.begin() + Offset + Length);
    patchExpression(
        Object,
        MutableArrayRef(reinterpret_cast<uint8_t *>(Out.end() - Length),
                        Length),
        Size, NoOffset, /*IsVariable=*/false);
    Offset += Length;
  }
  appendAddress(Out, 0, Size);
  appendAddress(Out, 0, Size);
}

/// Gathers what the layout needs to know about the kept entries of \p Unit:
/// the strings they use, their range and location lists, which are translated
/// here, which of their references leave the unit, and the names they are
/// found by.
void Scanner::collectFromKeptEntries(const UnitInfo &Unit) {
  useAbbreviations(Unit.AbbrevOffset);
  // The tables that the unit's entries have indexes for.
  IndexTable *Strings =
      Unit.StrOffsetsBase
          ? &ObjectFile::tableAt(Object.StrTables, Unit.StrOffsetsBase, 4)
          : nullptr;
  IndexTable *Addresses =
      Unit.AddrBase ? &ObjectFile::tableAt(Object.AddrTables, Unit.AddrBase,
                                           Unit.AddressSize)
                    : nullptr;
  IsRefAddrForm.clear();
  IsRefAddrForm.resize(Abbreviations.allAttributes().size());
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
      bool IsSwiftType = false;
      bool InFunction = !IsUnitEntry && Scopes.back().InFunction;
      // Whether the entry is a variable that dsymutil has in the table of
      // names: a constant outside of functions, or a variable at an address
      // that is in the binary.
      bool IsVariable = Abbrev.Tag == dwarf::DW_TAG_variable ||
                        Abbrev.Tag == dwarf::DW_TAG_constant;
      bool IsNamedVariable = false;

      for (const AttributeSpec &Spec : Abbreviations.attributes(Abbrev)) {
        const uint8_t *Value = P;
        uint64_t Size = formSize(Spec.Form, P, Unit.Version, Unit.AddressSize);
        P += Size;
        if (Spec.Form == dwarf::DW_FORM_strp) {
          Object.Strings.Offsets.push_back(read32le(Value));
        } else if (isStringIndex(Spec.Form)) {
          Strings->use(readIndex(Spec.Form, Value));
          Object.Strings.Offsets.push_back(
              stringOffset(Object, Spec.Form, Value, Unit.StrOffsetsBase));
        } else if (isAddressIndex(Spec.Form)) {
          // The unit's own DW_AT_low_pc is not an index in the output.
          if (Value != Base + Unit.LowPcOffset)
            Addresses->use(readIndex(Spec.Form, Value));
        } else if (Addresses && Spec.Form == dwarf::DW_FORM_exprloc) {
          forEachAddressOperand(
              blockContents(Spec.Form, Value), Unit.AddressSize,
              [&](size_t Offset, unsigned, bool IsIndex) {
                const uint8_t *Operand =
                    blockContents(Spec.Form, Value).data() + Offset;
                uint64_t Index = readULEB(Operand);
                if (IsIndex && addressSlot(Object, Index, Unit.AddressSize,
                                           Unit.AddrBase) != NoOffset)
                  Addresses->use(Index);
              });
        } else if (Spec.Form == dwarf::DW_FORM_ref4 && Unit.Version >= 3) {
          // A reference to an entry of another unit cannot stay relative to
          // this one. All attributes with the abbreviation follow.
          size_t Index = &Spec - Abbreviations.allAttributes().data();
          if (!IsRefAddrForm[Index] &&
              Object.findRedirect(Unit.InOffset + read32le(Value))) {
            Object.RefAddrForms.push_back(Spec.FormOffset);
            IsRefAddrForm.set(Index);
          }
        }

        switch (Spec.Attribute) {
        case dwarf::DW_AT_name:
        case dwarf::DW_AT_linkage_name:
        case dwarf::DW_AT_MIPS_linkage_name:
        case dwarf::DW_AT_specification:
        case dwarf::DW_AT_abstract_origin:
          noteName(Names, Spec, Value, Size, Unit.InOffset,
                   Unit.StrOffsetsBase);
          break;
        case dwarf::DW_AT_low_pc: {
          if (!isAddress(Spec.Form))
            break;
          const AddressRange *Code =
              Function ? Function
                       : Object.findContaining(
                             addressValue(Object, Spec.Form, Value,
                                          Unit.AddressSize, Unit.AddrBase));
          HasCode = Code != nullptr;
          if (Abbrev.Tag == dwarf::DW_TAG_subprogram)
            Function = Code;
          break;
        }
        case dwarf::DW_AT_ranges: {
          // Where the list is, and where the output gets it: an entry names
          // it by its offset or, in DWARF 5, by its index. If two entries
          // have the same index, the list of the second one stands.
          uint64_t List;
          SmallVectorImpl<char> *Out = &Object.RangesData;
          if (Spec.Form == dwarf::DW_FORM_rnglistx) {
            ListTable &Table = ObjectFile::tableAt(
                Object.RangeTables, Unit.RngListsBase, Unit.AddressSize);
            uint64_t Index = readIndex(Spec.Form, Value);
            Table.use(Index);
            Table.SlotLists.emplace_back(Index, Table.Lists.size());
            List = indexedList(Object.RngLists, Unit.RngListsBase, Index);
            Out = &Table.Lists;
          } else if (isSectionOffset(Spec.Form, Unit.Version)) {
            if (Unit.Version >= 5)
              Out =
                  &ObjectFile::tableAt(Object.RangeTables, 0, Unit.AddressSize)
                       .Lists;
            Object.RangeListOffsets.push_back(Out->size());
            List = read32le(Value);
          } else {
            break;
          }
          if (!IsUnitEntry) {
            HasCode |= addRangeList(Unit, List, Function, *Out) || Function;
            break;
          }
          // The unit's own list is that of its functions.
          for (std::pair<uint64_t, uint64_t> R :
               ArrayRef(Object.UnitRanges)
                   .slice(Unit.FirstAddressRange, Unit.NumAddressRanges))
            addRangeListEntry(Unit, *Out, R.first, R.second);
          endRangeList(Unit, *Out);
          break;
        }
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
          if (Spec.Form == dwarf::DW_FORM_loclistx) {
            ListTable &Table = ObjectFile::tableAt(
                Object.LocTables, Unit.LocListsBase, Unit.AddressSize);
            uint64_t Index = readIndex(Spec.Form, Value);
            Table.use(Index);
            Table.SlotLists.emplace_back(Index, Table.Lists.size());
            addLocationList(
                Unit, indexedList(Object.LocLists, Unit.LocListsBase, Index),
                Function, &Table);
          } else if (isLocationList(Spec, Unit.Version)) {
            ListTable *Table = Unit.Version >= 5
                                   ? &ObjectFile::tableAt(Object.LocTables, 0,
                                                          Unit.AddressSize)
                                   : nullptr;
            Object.LocListOffsets.push_back(Table ? Table->Lists.size()
                                                  : Object.LocData.size());
            addLocationList(Unit, read32le(Value), Function, Table);
          } else if (IsVariable && Spec.Attribute == dwarf::DW_AT_location &&
                     isExpression(Spec, Unit.Version)) {
            // A pointer to the address, as opposed to a variable at it, does
            // not count.
            ArrayRef<uint8_t> Location = blockContents(Spec.Form, Value);
            forEachAddressOperand(
                Location, Unit.AddressSize,
                [&](size_t Offset, unsigned Size, bool IsIndex) {
                  uint64_t Address;
                  if (findOperand(Object, Location.data() + Offset, Size,
                                  IsIndex, Unit.AddressSize, Unit.AddrBase,
                                  Location.data() + Offset - Base, Address))
                    IsNamedVariable |=
                        isMemoryLocation(Location, Unit.AddressSize);
                });
          }
          break;
        case dwarf::DW_AT_APPLE_runtime_class: {
          uint64_t Language = readUnsigned(Spec.Form, Value, Size);
          IsSwiftType = Language == dwarf::DW_LANG_Swift;
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
                Names.Name != NoString ? Names.Name : AnonymousNamespace, Entry,
                Abbrev.Tag);
      } else if (Abbrev.Tag == dwarf::DW_TAG_imported_declaration) {
        // An alias of a namespace.
        if (Names.Name != NoString)
          addName(Object.Namespaces, Names.Name, Entry, Abbrev.Tag);
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
        // A Swift type is also found by its mangled name, which it has as
        // its linkage name: that is how lldb looks for the type of a value
        // that the Swift runtime has told it the type of.
        StringRef LinkageName = TypeNames.LinkageName == NoString
                                    ? ""
                                    : Object.string(TypeNames.LinkageName);
        if (IsSwiftType && !LinkageName.empty() && LinkageName != Name) {
          Object.Types.push_back({TypeNames.LinkageName, Entry,
                                  djbHash(LinkageName), Abbrev.Tag,
                                  /*IsObjCImplementation=*/false});
          useExtraString(TypeNames.LinkageName);
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
  if (Object.Info.size() >= RefIsNode) {
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
      Object.DwarfVersion =
          std::max<unsigned>(Object.DwarfVersion, Unit.Header.Version);
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
  AbbrevOffset = UINT32_MAX;
  if (Nodes.size() >= NoNode) {
    Object.Warning = "__debug_info has too many entries";
    Nodes.clear();
  }
  // Like dsymutil, take nothing from an object file that has no code and no
  // variable in the binary. A clang module never has any.
  if (FunctionRanges.empty() && !HasLiveVariable && !Object.IsModule)
    Nodes.clear();
  if (Nodes.empty())
    return;

  markKept();
  for (const Node &N : Nodes)
    NumIdentities += N.IsKept && N.IsShared;
}

void Scanner::resolve(const OwnerTable &Spares, bool WithNameTags) {
  this->WithNameTags = WithNameTags;
  if (Nodes.empty())
    return;
  // What markShared() decided stands unless a member that this object file
  // found missing is in an earlier object file's spare copy: then the
  // references to it go there, and less is kept here.
  if (llvm::any_of(SpareMembers, [&](const SpareMember &S) {
        return OwnerTable::objectOf(Spares.lookup(S.Identity)) != Index;
      })) {
    this->Spares = &Spares;
    Object.Redirects.clear();
    for (auto &[Node, NodeMembers] : ForeignMembers)
      for (Member &M : NodeMembers)
        M.IsRedirected = false;
    markKept();
  }
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
  // A table of string offsets is a header of 8 bytes and the offsets. A
  // table of addresses is the same.
  for (IndexTable &Table : Object.StrTables) {
    Table.finish();
    Table.OutBase = Object.StrOffsetsSize + 8;
    Object.StrOffsetsSize += 8 + 4 * uint64_t(Table.size());
  }
  for (IndexTable &Table : Object.AddrTables) {
    Table.finish();
    Table.OutBase = Object.AddrSize + 8;
    Object.AddrSize += 8 + uint64_t(Table.SlotSize) * Table.size();
  }
  for (ListTable &Table : Object.RangeTables) {
    Table.finish();
    Table.OutBase = Object.RngListsSize + 12;
    Object.RngListsSize += Table.outSize();
  }
  for (ListTable &Table : Object.LocTables) {
    Table.finish();
    Table.OutBase = Object.LocListsSize + 12;
    Object.LocListsSize += Table.outSize();
  }
  for (UsedStrings *Strings : {&Object.Strings, &Object.LineStrings}) {
    llvm::sort(Strings->Offsets);
    Strings->Offsets.erase(llvm::unique(Strings->Offsets),
                           Strings->Offsets.end());
  }
  llvm::sort(Object.RefAddrForms);
  Object.RefAddrForms.erase(llvm::unique(Object.RefAddrForms),
                            Object.RefAddrForms.end());
  if (const char *Problem = linkFrameTable(Object))
    Object.Warning = Problem;
}

/// The path of the clang module that the unit with the header \p Header at
/// \p Offset of \p Object's __debug_info refers to, or an empty string if
/// it refers to none. Such a unit has DW_AT_dwo_name, or DW_AT_GNU_dwo_name
/// from before DWARF 5, in its own entry, and no other entry.
static std::string moduleReference(const ObjectFile &Object, uint32_t Offset,
                                   const UnitHeader &Header) {
  const uint8_t *P = Object.Info.bytes_begin() + Offset + Header.HeaderSize;
  const uint8_t *End = Object.Info.bytes_begin() + Offset + Header.Size;
  if (P >= End || Header.AbbrevOffset >= Object.Abbrev.size())
    return std::string();
  uint64_t Code = readULEB(P);

  // Only the entry's abbreviation is needed, which is the first of its
  // table or close to that: do not read the table. An item is at least a
  // byte, and the checks happen once per item, so that reads stay within a
  // few bytes of the section.
  const uint8_t *A = Object.Abbrev.bytes_begin() + Header.AbbrevOffset;
  const uint8_t *AEnd = Object.Abbrev.bytes_end();
  auto NextSpec = [&](uint64_t &Attribute, uint64_t &Form) {
    if (AEnd - A < 2)
      return false;
    Attribute = readULEB(A);
    Form = readULEB(A);
    if (Form == dwarf::DW_FORM_implicit_const)
      skipLEB(A);
    return Attribute != 0 || Form != 0;
  };
  uint64_t Attribute, Form;
  while (true) {
    if (AEnd - A < 3)
      return std::string();
    uint64_t AbbrevCode = readULEB(A);
    skipLEB(A);
    bool HasChildren = *A++;
    if (AbbrevCode == Code) {
      if (HasChildren)
        return std::string();
      break;
    }
    if (AbbrevCode == 0)
      return std::string();
    while (NextSpec(Attribute, Form))
      ;
  }

  // The file's name can be relative to the unit's directory, and both can
  // be indexes into a table that a later attribute gives the place of.
  std::pair<uint16_t, const uint8_t *> Name = {0, nullptr}, Directory = Name;
  uint32_t StrOffsetsBase = 0;
  while (NextSpec(Attribute, Form)) {
    uint64_t Size = formSize(Form, P, Header.Version, Header.AddressSize);
    if (Size > uint64_t(End - P))
      return std::string();
    if (Attribute == dwarf::DW_AT_dwo_name ||
        Attribute == dwarf::DW_AT_GNU_dwo_name)
      Name = {Form, P};
    else if (Attribute == dwarf::DW_AT_comp_dir)
      Directory = {Form, P};
    else if (Attribute == dwarf::DW_AT_str_offsets_base &&
             Form == dwarf::DW_FORM_sec_offset)
      StrOffsetsBase = read32le(P);
    P += Size;
  }
  auto Text = [&](std::pair<uint16_t, const uint8_t *> Value) -> StringRef {
    if (!Value.second)
      return StringRef();
    if (Value.first == dwarf::DW_FORM_string)
      return reinterpret_cast<const char *>(Value.second);
    uint32_t String =
        stringOffset(Object, Value.first, Value.second, StrOffsetsBase);
    return String == NoString ? StringRef() : Object.string(String);
  };
  StringRef File = Text(Name);
  if (File.empty())
    return std::string();
  SmallString<256> Path;
  if (sys::path::is_relative(File))
    Path = Text(Directory);
  sys::path::append(Path, File);
  return std::string(Path);
}

void dsymutil2::addClangModules(InputFiles &Inputs, const DebugMap &Map,
                                StringRef PrependPath) {
  PhaseTimer Timer("find modules");
  StringSet<> Seen;
  // The modules that the files of one round refer to are the files of the
  // next.
  for (size_t First = 0; First != Inputs.Objects.size();) {
    size_t End = Inputs.Objects.size();
    std::vector<std::vector<std::string>> References(End - First);
    parallelFor(First, End, [&](size_t I) {
      const ObjectFile &Object = Inputs.Objects[I];
      UnitHeader Header;
      for (uint64_t Offset = 0; Offset < Object.Info.size() &&
                                !readUnitHeader(Object.Info, Offset, Header);
           Offset += Header.Size) {
        std::string Path = moduleReference(Object, Offset, Header);
        if (!Path.empty())
          References[I - First].push_back(std::move(Path));
      }
    });
    std::vector<std::string> Paths;
    for (std::vector<std::string> &OfObject : References)
      for (std::string &Path : OfObject)
        if (Seen.insert(Path).second)
          Paths.push_back(std::move(Path));
    First = End;
    openModules(Inputs, Map, Paths, PrependPath);
  }
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
  for (const ObjectFile &Object : Objects)
    UseDebugNames |= Object.DwarfVersion >= 5;
  size_t NumIdentities = 0;
  for (const std::unique_ptr<Scanner> &S : Scanners)
    NumIdentities += S->numIdentities();
  OwnerTable Owners(NumIdentities);
  {
    PhaseTimer Timer("find owners");
    parallelFor(0, Objects.size(),
                [&](size_t I) { Scanners[I]->recordOwners(Owners); });
  }
  std::atomic<size_t> NumMembers{0};
  {
    PhaseTimer Timer("find members");
    parallelFor(0, Objects.size(), [&](size_t I) {
      NumMembers += Scanners[I]->findOwnedMembers(Owners);
    });
  }
  OwnerTable Members(NumMembers);
  {
    PhaseTimer Timer("record members");
    parallelFor(0, Objects.size(),
                [&](size_t I) { Scanners[I]->recordMembers(Members); });
  }
  std::atomic<size_t> NumSpares{0};
  {
    PhaseTimer Timer("mark");
    parallelFor(0, Objects.size(), [&](size_t I) {
      NumSpares += Scanners[I]->markShared(Owners, Members);
    });
  }
  OwnerTable Spares(NumSpares);
  {
    PhaseTimer Timer("record spare members");
    parallelFor(0, Objects.size(),
                [&](size_t I) { Scanners[I]->recordSpareMembers(Spares); });
  }
  {
    PhaseTimer Timer("resolve");
    parallelFor(0, Objects.size(), [&](size_t I) {
      Scanners[I]->resolve(Spares, UseDebugNames);
      Scanners[I].reset();
    });
  }
}

//===----------------------------------------------------------------------===//
// Layout.
//===----------------------------------------------------------------------===//

void DwarfLinker::mergeStrings() {
  StrSize = dsymutil2::mergeStrings(Objects, &ObjectFile::Strings);
  LineStrSize = dsymutil2::mergeStrings(Objects, &ObjectFile::LineStrings);
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
    Object.StrOffsetsOffset = StrOffsetsSize;
    StrOffsetsSize += Object.StrOffsetsSize;
    Object.AddrOffset = AddrSize;
    AddrSize += Object.AddrSize;
    Object.RngListsOffset = RngListsSize;
    RngListsSize += Object.RngListsSize;
    Object.LocListsOffset = LocListsSize;
    LocListsSize += Object.LocListsSize;
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
      LocSize > UINT32_MAX || FrameSize > UINT32_MAX ||
      StrOffsetsSize > UINT32_MAX || AddrSize > UINT32_MAX ||
      RngListsSize > UINT32_MAX || LocListsSize > UINT32_MAX)
    fatal("the debug info does not fit in 4 GB sections");
  if (InfoSize == 0)
    return;
  // Now it is known where the entries are that references are redirected
  // to.
  parallelFor(0, Objects.size(), [&](size_t I) {
    for (Redirect &R : Objects[I].Redirects) {
      const ObjectFile &Owner = Objects[R.OwnerObject];
      R.Out = Owner.InfoOffset + Owner.infoOutOffset(R.OwnerOffset);
    }
  });
  {
    PhaseTimer Timer("accelerator tables");
    if (UseDebugNames) {
      DebugNames = buildDebugNames(Objects);
    } else {
      AccelTables[0] = buildNameTable(Objects, &ObjectFile::Names);
      AccelTables[1] = buildNameTable(Objects, &ObjectFile::Namespaces);
      AccelTables[2] = buildTypeTable(Objects);
      AccelTables[3] = buildNameTable(Objects, &ObjectFile::ObjCClasses);
    }
  }
  // A Mach-O section header has 32 bits for the section's place in the
  // file, and not every reader recovers the rest (see DsymWriter): the lldb
  // of Xcode 26 looks for a section that starts past 4 GiB in the wrong
  // place. So the sections are in the order of how much a debugger needs
  // them. What ends up past 4 GiB in a large dSYM is what it does best
  // without: the tables first, which lldb makes itself if it finds none of
  // them (with some of them, a type lookup is its last), then the location
  // lists of variables, for which it shows nonsense then.
  //
  // __debug_str is the largest section of a large dSYM and has to be found.
  // The writer puts it behind the others as far as it still starts in the
  // first 4 GiB then, the earlier ones here first. In the dSYM of the
  // Chromium Framework with full debug info (6.0 GiB) those are all, and
  // nothing starts past 4 GiB; in that of an official build (6.6 GiB) all
  // but the tables.
  if (LineSize)
    LineSection = Writer.addSection("__debug_line", LineSize);
  // An empty string is always there.
  if (LineStrSize > 1)
    LineStrSection = Writer.addSection("__debug_line_str", LineStrSize);
  if (RangesSize)
    RangesSection = Writer.addSection("__debug_ranges", RangesSize);
  if (RngListsSize)
    RngListsSection = Writer.addSection("__debug_rnglists", RngListsSize);
  InfoSection = Writer.addSection("__debug_info", InfoSize);
  AbbrevSection = Writer.addSection("__debug_abbrev", AbbrevSize);
  if (StrOffsetsSize)
    StrOffsetsSection = Writer.addSection("__debug_str_offs", StrOffsetsSize);
  if (AddrSize)
    AddrSection = Writer.addSection("__debug_addr", AddrSize);
  if (ArangesSize)
    ArangesSection = Writer.addSection("__debug_aranges", ArangesSize);
  if (FrameSize)
    FrameSection = Writer.addSection("__debug_frame", FrameSize);
  if (LocSize)
    LocSection = Writer.addSection("__debug_loc", LocSize);
  if (LocListsSize)
    LocListsSection = Writer.addSection("__debug_loclists", LocListsSize);
  static const char *const AccelNames[] = {"__apple_names", "__apple_namespac",
                                           "__apple_types", "__apple_objc"};
  if (UseDebugNames)
    Writer.addSection("__debug_names", DebugNames);
  for (size_t I = 0; I != std::size(AccelTables) && !UseDebugNames; ++I)
    Writer.addSection(AccelNames[I], AccelTables[I], /*IsWithPrevious=*/I != 0);
  if (StrSize) {
    StrSection = Writer.addSection("__debug_str", StrSize);
    Writer.setLastReachableSection(StrSection);
  }
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
  void patchRange(const UnitInfo &Unit, const KeptRange &Range, uint64_t Size);
  uint64_t sectionOffset(uint32_t InOffset) const;

  const ObjectFile &Object;
  uint8_t *Out;
  AbbreviationTable Abbreviations;
  /// The live function that the entries being patched are in, if any.
  const AddressRange *Function = nullptr;
  /// The tables that the unit's indexes of strings, of addresses, of range
  /// lists and of location lists are for, if it has them.
  const IndexTable *Strings = nullptr;
  const IndexTable *Addresses = nullptr;
  const ListTable *RangeLists = nullptr;
  const ListTable *LocLists = nullptr;
  /// The tables with the lists that entries of DWARF 5 units name by their
  /// offset.
  const ListTable *OffsetRangeLists = nullptr;
  const ListTable *OffsetLocLists = nullptr;
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
  if (const Redirect *R = Object.findRedirect(InOffset))
    return R->Out;
  return Object.InfoOffset + Object.infoOutOffset(InOffset);
}

void UnitWriter::write(const UnitInfo &Unit) {
  uint8_t *Header = Out + Unit.OutOffset;
  memcpy(Header, Object.Info.data() + Unit.InOffset, Unit.HeaderSize);
  write32le(Header, Unit.OutSize - 4);
  write32le(Header + (Unit.Version >= 5 ? 8 : 6),
            Object.AbbrevOffset + Unit.AbbrevOffset);
  Strings = ObjectFile::tableAt(Object.StrTables, Unit.StrOffsetsBase);
  Addresses = ObjectFile::tableAt(Object.AddrTables, Unit.AddrBase);
  RangeLists = Unit.RngListsBase
                   ? ObjectFile::tableAt(Object.RangeTables, Unit.RngListsBase)
                   : nullptr;
  LocLists = Unit.LocListsBase
                 ? ObjectFile::tableAt(Object.LocTables, Unit.LocListsBase)
                 : nullptr;
  OffsetRangeLists = ObjectFile::tableAt(Object.RangeTables, 0);
  OffsetLocLists = ObjectFile::tableAt(Object.LocTables, 0);

  // The scan has parsed this table before, successfully.
  Abbreviations.parse(Object.Abbrev, Unit.AbbrevOffset);
  for (AttributeSpec &Spec : Abbreviations.allAttributes()) {
    if (Spec.Form == dwarf::DW_FORM_ref4 &&
        llvm::binary_search(Object.RefAddrForms, Spec.FormOffset))
      Spec.Form = FormRef4AsRefAddr;
    if (Unit.LowPcSize && Spec.FormOffset == Unit.LowPcFormOffset)
      Spec.Form = dwarf::DW_FORM_addr;
  }
  AtUnitEntry = true;
  NextRangeList = Unit.FirstRangeList;
  NextLocList = Unit.FirstLocList;
  for (const KeptRange &Range :
       ArrayRef(Object.KeptRanges).slice(Unit.FirstRange, Unit.NumRanges)) {
    const char *In = Object.Info.data() + Range.InBegin;
    uint64_t Size = Range.InEnd - Range.InBegin;
    if (AtUnitEntry && Unit.LowPcSize) {
      // The unit's own entry, with room for an address where the index of
      // one was.
      uint64_t Before = Unit.LowPcOffset - Range.InBegin;
      uint64_t After = Before + Unit.LowPcSize;
      memcpy(Out + Range.Out, In, Before);
      memcpy(Out + Range.Out + Before + Unit.AddressSize, In + After,
             Size - After);
      Size += Unit.AddressSize - Unit.LowPcSize;
    } else {
      memcpy(Out + Range.Out, In, Size);
    }
    patchRange(Unit, Range, Size);
  }
}

/// Patches the entries of the copy of \p Range, which has \p Size bytes.
void UnitWriter::patchRange(const UnitInfo &Unit, const KeptRange &Range,
                            uint64_t Size) {
  uint8_t *P = Out + Range.Out;
  uint8_t *End = P + Size;
  // Where the copy is, relative to the original.
  const uint8_t *Original = Object.Info.bytes_begin() + Range.InBegin;
  const uint8_t *Copy = P;
  const Abbreviation *Entry = nullptr;
  // Set when the entry's low_pc is not in the binary: the entry describes
  // code that did not survive the link.
  bool IsDead = false;
  bool IsUnitEntry = false;
  // The entry's low_pc in the object file, if it is in the function that the
  // entry is in or describes.
  std::optional<uint64_t> LowPcInFunction;
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
        LowPcInFunction.reset();
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
        case dwarf::DW_FORM_addr:
        case dwarf::DW_FORM_addrx:
        case dwarf::DW_FORM_addrx1:
        case dwarf::DW_FORM_addrx2:
        case dwarf::DW_FORM_addrx3:
        case dwarf::DW_FORM_addrx4: {
          uint64_t Address = addressValue(Object, Spec.Form, P,
                                          Unit.AddressSize, Unit.AddrBase);
          const AddressRange *Range = nullptr;
          if (Spec.Attribute == dwarf::DW_AT_low_pc) {
            Range = Object.findContaining(Address);
            if (!Range)
              IsDead = true;
            else if (Entry->Tag == dwarf::DW_TAG_subprogram)
              Function = Range;
            if (Function && Address >= Function->Begin &&
                Address < Function->End)
              LowPcInFunction = Address;
          } else if (!IsDead) {
            // An address in a function can be the function's end, which is
            // where the next function starts: try the function first.
            // A function, and what is in it, does not reach past where the
            // function ends in the binary: see below.
            if (LowPcInFunction && Spec.Attribute == dwarf::DW_AT_high_pc)
              Address = std::min(Address, Function->End);
            if (Function && Address >= Function->Begin &&
                Address <= Function->End)
              Range = Function;
            else
              Range = Object.findContaining(Address);
          }
          // The slot of an index says where the address is in the binary.
          if (Spec.Form == dwarf::DW_FORM_addr)
            WriteAddress(P, Range ? Address - Range->Begin + Range->Linked : 0);
          else
            writeIndex(Spec.Form, P, Size,
                       Addresses->outIndex(readIndex(Spec.Form, P)));
          break;
        }
        case dwarf::DW_FORM_data1:
        case dwarf::DW_FORM_data2:
        case dwarf::DW_FORM_data4:
        case dwarf::DW_FORM_data8:
          // The length of code that is not there.
          if (IsDead && Spec.Attribute == dwarf::DW_AT_high_pc)
            memset(P, 0, Size);
          // A function can be described as longer than its symbol is, which
          // the linker placed without what followed it in the object file.
          // If another symbol is in the way in the binary, the function
          // ends there, as with dsymutil: the unit's range and the line
          // table go by that as well. So does what is in the function.
          else if (LowPcInFunction && Spec.Attribute == dwarf::DW_AT_high_pc &&
                   readUnsigned(Spec.Form, P, Size) >
                       Function->End - *LowPcInFunction)
            writeUnsigned(P, Size, Function->End - *LowPcInFunction);
          [[fallthrough]];
        case dwarf::DW_FORM_sec_offset:
          if (!isSectionOffset(Spec.Form, Unit.Version))
            break;
          if (Spec.Attribute == dwarf::DW_AT_stmt_list && Unit.HasLineTable)
            write32le(P, Object.LineOffset + Unit.LineOffset);
          else if (Spec.Attribute == dwarf::DW_AT_str_offsets_base && Strings)
            write32le(P, Object.StrOffsetsOffset + Strings->OutBase);
          else if (Spec.Attribute == dwarf::DW_AT_addr_base && Addresses)
            write32le(P, Object.AddrOffset + Addresses->OutBase);
          else if (Spec.Attribute == dwarf::DW_AT_rnglists_base && RangeLists)
            write32le(P, Object.RngListsOffset + RangeLists->OutBase);
          else if (Spec.Attribute == dwarf::DW_AT_loclists_base && LocLists)
            write32le(P, Object.LocListsOffset + LocLists->OutBase);
          else if (Spec.Attribute == dwarf::DW_AT_ranges)
            // The scan translated the lists in the order of their entries.
            write32le(P, (Unit.Version >= 5 ? Object.RngListsOffset +
                                                  OffsetRangeLists->OutBase
                                            : Object.RangesOffset) +
                             Object.RangeListOffsets[NextRangeList++]);
          else if (isLocationList(Spec, Unit.Version))
            write32le(P, (Unit.Version >= 5
                              ? Object.LocListsOffset + OffsetLocLists->OutBase
                              : Object.LocOffset) +
                             Object.LocListOffsets[NextLocList++]);
          break;
        case dwarf::DW_FORM_rnglistx:
          writeIndex(Spec.Form, P, Size,
                     RangeLists->outIndex(readIndex(Spec.Form, P)));
          break;
        case dwarf::DW_FORM_loclistx:
          writeIndex(Spec.Form, P, Size,
                     LocLists->outIndex(readIndex(Spec.Form, P)));
          break;
        case dwarf::DW_FORM_exprloc:
        case dwarf::DW_FORM_block:
        case dwarf::DW_FORM_block1:
        case dwarf::DW_FORM_block2:
        case dwarf::DW_FORM_block4: {
          if (!isExpression(Spec, Unit.Version))
            break;
          ArrayRef<uint8_t> Expression = blockContents(Spec.Form, P);
          patchExpression(
              Object,
              MutableArrayRef(const_cast<uint8_t *>(Expression.data()),
                              Expression.size()),
              Unit.AddressSize,
              Expression.data() - Copy + Original - Object.Info.bytes_begin(),
              Entry->Tag == dwarf::DW_TAG_variable ||
                  Entry->Tag == dwarf::DW_TAG_constant,
              Unit.AddrBase, Addresses, &Unit);
          break;
        }
        case dwarf::DW_FORM_strp:
          write32le(P, Object.Strings.outOffset(read32le(P)));
          break;
        case dwarf::DW_FORM_strx:
        case dwarf::DW_FORM_strx1:
        case dwarf::DW_FORM_strx2:
        case dwarf::DW_FORM_strx3:
        case dwarf::DW_FORM_strx4:
          writeIndex(Spec.Form, P, Size,
                     Strings->outIndex(readIndex(Spec.Form, P)));
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

/// Writes the tables of range lists or of location lists of \p Object to
/// \p Out, its contribution to their section.
static void writeListTables(const ObjectFile &Object,
                            const std::vector<ListTable> &Tables, char *Out) {
  for (const ListTable &Table : Tables) {
    // The header: the size of what follows it, the version, the sizes of an
    // address and of a segment selector, and the number of offsets. Then
    // the offsets of the lists, from where they start, and the lists.
    char *Offsets = Out + Table.OutBase;
    write32le(Offsets - 12, Table.outSize() - 4);
    write16le(Offsets - 8, 5);
    Offsets[-6] = Table.AddressSize;
    Offsets[-5] = 0;
    write32le(Offsets - 4, Table.size());
    char *Lists = Offsets + 4 * Table.size();
    for (auto [Slot, List] : Table.SlotLists)
      write32le(Offsets + 4 * Table.outIndex(Slot), 4 * Table.size() + List);
    memcpy(Lists, Table.Lists.data(), Table.Lists.size());
    for (auto [Offset, AddrBase] : Table.AddressIndexes) {
      uint8_t *Index = reinterpret_cast<uint8_t *>(Lists) + Offset;
      const uint8_t *End = Index;
      uint64_t Slot = readULEB(End);
      writeIndex(
          dwarf::DW_FORM_addrx, Index, End - Index,
          ObjectFile::tableAt(Object.AddrTables, AddrBase)->outIndex(Slot));
    }
  }
}

void DwarfLinker::write(DsymWriter &Writer) {
  if (InfoSize == 0)
    return;
  // First the sections whose contents the scan has made, so that the writer
  // has something to write to the file while the others are filled.
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    if (!Object.LineData.empty()) {
      char *Line = Writer.sectionData(LineSection) + Object.LineOffset;
      memcpy(Line, Object.LineData.data(), Object.LineData.size());
      for (uint32_t Offset : Object.LineDataStrings) {
        uint32_t String = read32le(Line + Offset);
        write32le(Line + Offset, String < Object.lineStringsOffset()
                                     ? Object.Strings.outOffset(String)
                                     : Object.LineStrings.outOffset(String));
      }
    }
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

  if (LineStrSize > 1) {
    writeStrings(Objects, &ObjectFile::LineStrings,
                 Writer.sectionData(LineStrSection));
    Writer.finishSection(LineStrSection);
  }
  if (StrSize) {
    writeStrings(Objects, &ObjectFile::Strings, Writer.sectionData(StrSection));
    Writer.finishSection(StrSection);
  }

  char *Info = Writer.sectionData(InfoSection);
  char *Abbrev = Writer.sectionData(AbbrevSection);
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    if (Object.Units.empty())
      return;
    // The tables of addresses: the size of what follows it, the version,
    // the size of an address and of a segment selector, and where the
    // addresses are in the binary.
    uint8_t *Addr =
        AddrSize
            ? reinterpret_cast<uint8_t *>(Writer.sectionData(AddrSection)) +
                  Object.AddrOffset
            : nullptr;
    for (const IndexTable &Table : Object.AddrTables) {
      uint8_t *Out = Addr + Table.OutBase;
      write32le(Out - 8, 4 + Table.SlotSize * Table.size());
      write16le(Out - 4, 5);
      Out[-2] = Table.SlotSize;
      Out[-1] = 0;
      Table.forEachSlot([&](uint32_t Slot) {
        uint64_t Offset = Table.Base + uint64_t(Table.SlotSize) * Slot;
        writeUnsigned(
            Out, Table.SlotSize,
            Object.linkedSlot(
                readAddress(Object.Addr.bytes_begin() + Offset, Table.SlotSize),
                Offset));
        Out += Table.SlotSize;
      });
    }
    writeListTables(Object, Object.RangeTables,
                    RngListsSize ? Writer.sectionData(RngListsSection) +
                                       Object.RngListsOffset
                                 : nullptr);
    writeListTables(Object, Object.LocTables,
                    LocListsSize ? Writer.sectionData(LocListsSection) +
                                       Object.LocListsOffset
                                 : nullptr);
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
    memcpy(Abbrev + Object.AbbrevOffset, Object.Abbrev.data(),
           Object.Abbrev.size());
    // The tables of string offsets: the size of what follows it, the
    // version, two bytes of padding, and where the strings went.
    for (const IndexTable &Table : Object.StrTables) {
      char *Out = Writer.sectionData(StrOffsetsSection) +
                  Object.StrOffsetsOffset + Table.OutBase;
      write32le(Out - 8, 4 + 4 * Table.size());
      write16le(Out - 4, 5);
      write16le(Out - 2, 0);
      Table.forEachSlot([&](uint32_t Slot) {
        write32le(Out, Object.Strings.outOffset(read32le(
                           Object.StrOffsets.data() + Table.Base + 4 * Slot)));
        Out += 4;
      });
    }
    for (uint32_t FormOffset : Object.RefAddrForms)
      Abbrev[Object.AbbrevOffset + FormOffset] = dwarf::DW_FORM_ref_addr;
    for (const UnitInfo &Unit : Object.Units)
      if (Unit.LowPcSize)
        Abbrev[Object.AbbrevOffset + Unit.LowPcFormOffset] =
            dwarf::DW_FORM_addr;
  });
}
