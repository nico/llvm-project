//===- Dwarf.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dwarf.h"

using namespace llvm;
using namespace llvm::dsymutil2;

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
      uint64_t Form = readULEB(P);
      if (Form == dwarf::DW_FORM_implicit_const)
        skipLEB(P);
      if (Attribute == 0 && Form == 0)
        break;
      if (Attribute > UINT16_MAX || Form > UINT16_MAX)
        return false;
      Attributes.push_back({uint16_t(Attribute), uint16_t(Form)});
    }
    Abbrev.NumAttributes = Attributes.size() - Abbrev.FirstAttribute;
  }
  return false;
}
