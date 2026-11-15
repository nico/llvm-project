//===- DwarfLinker.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Links the DWARF of the object files in three steps: scan() decides, object
// by object, which debugging information entries to keep; layout() gives
// every object's contribution a place in the output sections; write() copies
// the kept bytes there and patches the addresses and offsets in them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_DWARFLINKER_H
#define LLVM_TOOLS_DSYMUTIL2_DWARFLINKER_H

#include "ObjectFile.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
namespace dsymutil2 {
class DsymWriter;

class DwarfLinker {
public:
  explicit DwarfLinker(MutableArrayRef<ObjectFile> Objects)
      : Objects(Objects) {}

  void scan();
  /// Adds the output sections to \p Writer.
  void layout(DsymWriter &Writer);
  /// Fills in the sections. \p Writer must have created the file.
  void write(DsymWriter &Writer);

private:
  MutableArrayRef<ObjectFile> Objects;
  uint64_t InfoSize = 0;
  uint64_t AbbrevSize = 0;
  uint64_t StrSize = 0;
  unsigned InfoSection = 0;
  unsigned AbbrevSection = 0;
  unsigned StrSection = 0;
};

} // namespace dsymutil2
} // namespace llvm

#endif
