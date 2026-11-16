//===- LineTable.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_LINETABLE_H
#define LLVM_TOOLS_DSYMUTIL2_LINETABLE_H

#include "llvm/ADT/SmallVector.h"
#include <cstdint>

namespace llvm {
namespace dsymutil2 {
struct ObjectFile;

/// Appends to \p Object's LineData the line table at \p Offset of its
/// __debug_line, rewritten for the functions that are in the linked binary.
/// Returns null on success; otherwise what is wrong with the table, with
/// LineData unchanged.
const char *linkLineTable(ObjectFile &Object, uint64_t Offset);

} // namespace dsymutil2
} // namespace llvm

#endif
