//===- InputSection.h -------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_INPUT_SECTION_H
#define LLD_MACHO_INPUT_SECTION_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/BinaryFormat/MachO.h"

namespace lld {
namespace macho {

class InputFile;
class InputSection;
class Symbol;

struct Reloc {
  uint8_t Type;
  bool HasImplicitAddend;
  uint32_t Addend;
  uint32_t Offset;
  llvm::PointerUnion<Symbol *, InputSection *> Target;
};

class InputSection {
public:
  void writeTo(uint8_t *Buf);

  InputFile *File = nullptr;
  StringRef Name;

  // Every section is inside a segment. This is the name of the containing
  // segment of this section.
  StringRef SegName;

  ArrayRef<uint8_t> Data;
  uint32_t Align = 0;
  uint64_t Addr = 0;

  std::vector<Reloc> Relocs;
};

extern std::vector<InputSection *> InputSections;

} // namespace macho
} // namespace lld

#endif
