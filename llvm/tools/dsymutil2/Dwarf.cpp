//===- Dwarf.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dwarf.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"
#include "llvm/Support/DataExtractor.h"

using namespace llvm;
using namespace llvm::dsymutil2;

void dsymutil2::forEachAddressOperand(
    ArrayRef<uint8_t> Expression, unsigned AddressSize,
    function_ref<void(size_t, unsigned, bool)> OnAddress) {
  // Most expressions are a register or an offset from one. Do not decode an
  // expression that has none of the operations that take an address.
  if (llvm::none_of(Expression, [](uint8_t Byte) {
        return Byte == dwarf::DW_OP_addr || Byte == dwarf::DW_OP_addrx ||
               Byte == dwarf::DW_OP_constx || dwarf::isTlsAddressOp(Byte);
      }))
    return;
  DataExtractor Data(Expression, /*IsLittleEndian=*/true);
  // The constant that the operation before the current one pushed, if any.
  size_t ConstantOffset = 0;
  unsigned ConstantSize = 0;
  size_t Offset = 0;
  for (const DWARFExpression::Operation &Op :
       DWARFExpression(Data, AddressSize)) {
    if (Op.isError())
      return;
    // What claims to be an expression may not be one. Do not trust it with
    // an operand that is not there.
    if (Op.getEndOffset() > Expression.size())
      return;
    if (ConstantSize && dwarf::isTlsAddressOp(Op.getCode()))
      OnAddress(ConstantOffset, ConstantSize, /*IsIndex=*/false);
    ConstantSize = 0;
    switch (Op.getCode()) {
    case dwarf::DW_OP_addr:
      if (Op.getEndOffset() - Offset == 1 + AddressSize)
        OnAddress(Offset + 1, AddressSize, /*IsIndex=*/false);
      break;
    case dwarf::DW_OP_addrx:
    case dwarf::DW_OP_constx:
      // A constant from the table of addresses is one that the linker had
      // to make an address: that of a thread-local variable.
      OnAddress(Offset + 1, Op.getEndOffset() - Offset - 1, /*IsIndex=*/true);
      break;
    case dwarf::DW_OP_const2u:
    case dwarf::DW_OP_const2s:
    case dwarf::DW_OP_const4u:
    case dwarf::DW_OP_const4s:
    case dwarf::DW_OP_const8u:
    case dwarf::DW_OP_const8s:
      ConstantOffset = Offset + 1;
      ConstantSize = Op.getEndOffset() - ConstantOffset;
      break;
    default:
      break;
    }
    Offset = Op.getEndOffset();
  }
}

bool dsymutil2::isMemoryLocation(ArrayRef<uint8_t> Expression,
                                 unsigned AddressSize) {
  DataExtractor Data(Expression, /*IsLittleEndian=*/true);
  return DWARFExpression(Data, AddressSize).isMemoryLocation();
}

bool AbbreviationTable::parse(StringRef Section, uint64_t Offset) {
  Abbreviations.clear();
  Attributes.clear();
  if (Offset >= Section.size())
    return false;
  // Every item below is at least one byte, and the checks against End happen
  // once per item, so reads stay within a few bytes of the section.
  const uint8_t *P = Section.bytes_begin() + Offset;
  const uint8_t *End = Section.bytes_end();
  while (P < End) {
    uint64_t Code = readULEB(P);
    if (Code == 0)
      return true;
    // A table with huge codes is not one a compiler wrote.
    if (Code > (1 << 20) || P + 2 > End)
      return false;
    if (Abbreviations.size() <= Code)
      Abbreviations.resize(Code + 1);
    Abbreviation &Abbrev = Abbreviations[Code];
    Abbrev.Tag = readULEB(P);
    Abbrev.HasChildren = *P++;
    Abbrev.FirstAttribute = Attributes.size();
    if (Abbrev.Tag == 0)
      return false;
    while (true) {
      if (P + 2 > End)
        return false;
      uint64_t Attribute = readULEB(P);
      uint32_t FormOffset = P - Section.bytes_begin();
      uint64_t Form = readULEB(P);
      if (Form == dwarf::DW_FORM_implicit_const)
        skipLEB(P);
      if (Attribute == 0 && Form == 0)
        break;
      if (Attribute > UINT16_MAX || Form > UINT16_MAX)
        return false;
      Abbrev.HasInline |= Attribute == dwarf::DW_AT_inline;
      Attributes.push_back({uint16_t(Attribute), uint16_t(Form), FormOffset});
    }
    Abbrev.NumAttributes = Attributes.size() - Abbrev.FirstAttribute;
  }
  return false;
}
