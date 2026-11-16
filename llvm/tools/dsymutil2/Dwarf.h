//===- Dwarf.h --------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The little that is needed to walk DWARF debugging information entries
// without building anything: LEB128, abbreviation tables, and the sizes of
// attribute values.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_DWARF_H
#define LLVM_TOOLS_DSYMUTIL2_DWARF_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/Endian.h"
#include <vector>

namespace llvm {
namespace dsymutil2 {

inline uint64_t readULEB(const uint8_t *&P) {
  uint64_t Value = *P++;
  if (LLVM_LIKELY(Value < 0x80))
    return Value;
  Value &= 0x7f;
  for (unsigned Shift = 7; Shift < 64; Shift += 7) {
    uint8_t Byte = *P++;
    Value |= uint64_t(Byte & 0x7f) << Shift;
    if (Byte < 0x80)
      break;
  }
  return Value;
}

inline void skipLEB(const uint8_t *&P) {
  while (*P++ & 0x80)
    ;
}

struct AttributeSpec {
  uint16_t Attribute;
  uint16_t Form;
  /// Where the form is in the __debug_abbrev section.
  uint32_t FormOffset;
};

/// Not a form of DWARF. The linker gives it to an attribute that is a
/// DW_FORM_ref4 in the object file and a DW_FORM_ref_addr in the output,
/// because it can refer to an entry of another unit there. Both forms have
/// four bytes.
constexpr uint16_t FormRef4AsRefAddr = 0xff00 | dwarf::DW_FORM_ref4;

struct Abbreviation {
  uint16_t Tag = 0;
  bool HasChildren = false;
  /// Whether one of the attributes is DW_AT_inline.
  bool HasInline = false;
  uint32_t FirstAttribute = 0;
  uint32_t NumAttributes = 0;
};

/// The abbreviations of one unit.
class AbbreviationTable {
public:
  /// Parses the table at \p Offset of the __debug_abbrev section \p Section.
  /// Returns false if it is malformed or uses something unsupported.
  bool parse(StringRef Section, uint64_t Offset);

  /// The abbreviation with code \p Code, or null.
  const Abbreviation *find(uint64_t Code) const {
    return Code < Abbreviations.size() && Abbreviations[Code].Tag
               ? &Abbreviations[Code]
               : nullptr;
  }

  ArrayRef<AttributeSpec> attributes(const Abbreviation &Abbrev) const {
    return ArrayRef(Attributes)
        .slice(Abbrev.FirstAttribute, Abbrev.NumAttributes);
  }

  /// The attributes of all abbreviations.
  MutableArrayRef<AttributeSpec> allAttributes() { return Attributes; }

private:
  /// Indexed by code. Compilers number abbreviations densely from 1.
  std::vector<Abbreviation> Abbreviations;
  std::vector<AttributeSpec> Attributes;
};

/// DWARF 5 has forms whose value is the index of a slot in a table that the
/// unit has in another section. This is whether a value of form \p Form is
/// the index of a string.
inline bool isStringIndex(uint16_t Form) {
  // The forms of the versions before are the smaller numbers.
  if (LLVM_LIKELY(Form < dwarf::DW_FORM_strx))
    return false;
  switch (Form) {
  case dwarf::DW_FORM_strx:
  case dwarf::DW_FORM_strx1:
  case dwarf::DW_FORM_strx2:
  case dwarf::DW_FORM_strx3:
  case dwarf::DW_FORM_strx4:
    return true;
  default:
    return false;
  }
}

/// Whether a value of form \p Form is the index of an address.
inline bool isAddressIndex(uint16_t Form) {
  if (LLVM_LIKELY(Form < dwarf::DW_FORM_addrx))
    return false;
  switch (Form) {
  case dwarf::DW_FORM_addrx:
  case dwarf::DW_FORM_addrx1:
  case dwarf::DW_FORM_addrx2:
  case dwarf::DW_FORM_addrx3:
  case dwarf::DW_FORM_addrx4:
    return true;
  default:
    return false;
  }
}

/// Whether a value of form \p Form is an address, there or in a table.
inline bool isAddress(uint16_t Form) {
  return Form == dwarf::DW_FORM_addr || isAddressIndex(Form);
}

/// Whether an index of form \p Form has as many bytes as its value takes,
/// as opposed to a number of bytes that the form says.
inline bool isLEBIndex(uint16_t Form) {
  return Form == dwarf::DW_FORM_strx || Form == dwarf::DW_FORM_addrx;
}

/// Reads a value of a form that is an index.
inline uint64_t readIndex(uint16_t Form, const uint8_t *P) {
  switch (Form) {
  case dwarf::DW_FORM_strx1:
  case dwarf::DW_FORM_addrx1:
    return *P;
  case dwarf::DW_FORM_strx2:
  case dwarf::DW_FORM_addrx2:
    return support::endian::read16le(P);
  case dwarf::DW_FORM_strx3:
  case dwarf::DW_FORM_addrx3:
    return support::endian::read16le(P) | uint32_t(P[2]) << 16;
  case dwarf::DW_FORM_strx4:
  case dwarf::DW_FORM_addrx4:
    return support::endian::read32le(P);
  default:
    return readULEB(P);
  }
}

/// Writes \p Index over an index of \p Size bytes at \p P. \p Index must
/// not be larger than the one that was there: then it fits. A number of
/// variable size is padded to the size of the old one.
inline void writeIndex(uint16_t Form, uint8_t *P, uint64_t Size,
                       uint64_t Index) {
  for (uint64_t I = 0; I != Size; ++I) {
    if (!isLEBIndex(Form)) {
      P[I] = Index;
      Index >>= 8;
    } else {
      P[I] = (Index & 0x7f) | (I + 1 != Size ? 0x80 : 0);
      Index >>= 7;
    }
  }
}

constexpr uint64_t UnsupportedForm = ~uint64_t(0);

/// The size of the value of form \p Form at \p P in a DWARF32 unit of version
/// \p Version, or UnsupportedForm.
inline uint64_t formSize(uint16_t Form, const uint8_t *P, unsigned Version,
                         unsigned AddressSize) {
  using namespace dwarf;
  using namespace support::endian;
  switch (Form) {
  case DW_FORM_addr:
    return AddressSize;
  case DW_FORM_flag_present:
  case DW_FORM_implicit_const:
    return 0;
  case DW_FORM_data1:
  case DW_FORM_flag:
  case DW_FORM_ref1:
  case DW_FORM_strx1:
  case DW_FORM_addrx1:
    return 1;
  case DW_FORM_data2:
  case DW_FORM_ref2:
  case DW_FORM_strx2:
  case DW_FORM_addrx2:
    return 2;
  case DW_FORM_strx3:
  case DW_FORM_addrx3:
    return 3;
  case DW_FORM_data4:
  case DW_FORM_ref4:
  case FormRef4AsRefAddr:
  case DW_FORM_strp:
  case DW_FORM_sec_offset:
  case DW_FORM_strx4:
  case DW_FORM_addrx4:
    return 4;
  case DW_FORM_data8:
  case DW_FORM_ref8:
  case DW_FORM_ref_sig8:
    return 8;
  case DW_FORM_data16:
    return 16;
  case DW_FORM_ref_addr:
    return Version <= 2 ? AddressSize : 4;
  case DW_FORM_string:
    return strlen(reinterpret_cast<const char *>(P)) + 1;
  case DW_FORM_block1:
    return 1 + *P;
  case DW_FORM_block2:
    return 2 + read16le(P);
  case DW_FORM_block4:
    return 4 + uint64_t(read32le(P));
  case DW_FORM_block:
  case DW_FORM_exprloc: {
    const uint8_t *Q = P;
    uint64_t Length = readULEB(Q);
    return (Q - P) + Length;
  }
  case DW_FORM_sdata:
  case DW_FORM_udata:
  case DW_FORM_strx:
  case DW_FORM_addrx: {
    const uint8_t *Q = P;
    skipLEB(Q);
    return Q - P;
  }
  default:
    // Not handled yet: the other indexed forms of DWARF 5, DW_FORM_ref_udata
    // and DW_FORM_indirect, whose values cannot be patched in place, and the
    // forms for supplementary object files.
    return UnsupportedForm;
  }
}

/// Whether the value of attribute \p Spec in a unit of version \p Version is
/// a location expression. Before DWARF 4, expressions had the forms that
/// blocks of bytes have, which the value of a constant can still be.
inline bool isExpression(AttributeSpec Spec, unsigned Version) {
  if (Spec.Form == dwarf::DW_FORM_exprloc)
    return true;
  return Version < 4 && Spec.Attribute != dwarf::DW_AT_const_value &&
         (Spec.Form == dwarf::DW_FORM_block ||
          Spec.Form == dwarf::DW_FORM_block1 ||
          Spec.Form == dwarf::DW_FORM_block2 ||
          Spec.Form == dwarf::DW_FORM_block4);
}

/// The contents of the value of block form \p Form at \p P, without the
/// length in front of them.
inline ArrayRef<uint8_t> blockContents(uint16_t Form, const uint8_t *P) {
  using namespace support::endian;
  switch (Form) {
  case dwarf::DW_FORM_block1:
    return ArrayRef(P + 1, *P);
  case dwarf::DW_FORM_block2:
    return ArrayRef(P + 2, read16le(P));
  case dwarf::DW_FORM_block4:
    return ArrayRef(P + 4, read32le(P));
  default: {
    uint64_t Length = readULEB(P);
    return ArrayRef(P, Length);
  }
  }
}

/// Calls \p OnAddress(Offset, Size, IsIndex) for each operand of the
/// location expression \p Expression that is an address in the object file:
/// the operand of a DW_OP_addr, and a constant that the operation after it
/// takes for the address of a thread-local variable. With \p IsIndex, the
/// operand is the index of the address in the unit's table of addresses,
/// a number of variable size: DW_OP_addrx and DW_OP_constx of DWARF 5.
void forEachAddressOperand(
    ArrayRef<uint8_t> Expression, unsigned AddressSize,
    function_ref<void(size_t, unsigned, bool)> OnAddress);

/// Whether the location expression \p Expression says where an object is in
/// memory, as opposed to what its value is.
bool isMemoryLocation(ArrayRef<uint8_t> Expression, unsigned AddressSize);

/// Calls \p OnEntry(Abbreviation) for every entry in [P, End) and
/// \p OnAttribute(AttributeSpec, Value, Size) for each of its attributes.
/// The bytes must be whole entries of a unit of the given kind that are known
/// to be well-formed.
template <class EntryFnTy, class AttributeFnTy>
void forEachEntry(const uint8_t *P, const uint8_t *End,
                  const AbbreviationTable &Abbreviations, unsigned Version,
                  unsigned AddressSize, EntryFnTy OnEntry,
                  AttributeFnTy OnAttribute) {
  while (P < End) {
    uint64_t Code = readULEB(P);
    if (Code == 0)
      continue;
    const Abbreviation &Abbrev = *Abbreviations.find(Code);
    OnEntry(Abbrev);
    for (AttributeSpec Spec : Abbreviations.attributes(Abbrev)) {
      uint64_t Size = formSize(Spec.Form, P, Version, AddressSize);
      OnAttribute(Spec, P, Size);
      P += Size;
    }
  }
}

} // namespace dsymutil2
} // namespace llvm

#endif
