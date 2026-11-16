//===- StringTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Merges the strings that the kept debug info of all object files uses into
// one __debug_str section, in parallel and with a result that does not
// depend on how the threads were scheduled.
//
// Every distinct string is owned by the first object file that uses it, in
// debug map order. An object file's owned strings are laid out in the order
// of their offsets in that file, and the object files' blocks of owned
// strings follow each other in debug map order.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_STRINGTABLE_H
#define LLVM_TOOLS_DSYMUTIL2_STRINGTABLE_H

#include "ObjectFile.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
namespace dsymutil2 {

/// Gives the strings \p Uses of all of \p Objects their offsets in a
/// section that has each distinct string once. Returns the size of that
/// section.
uint64_t mergeStrings(MutableArrayRef<ObjectFile> Objects,
                      UsedStrings ObjectFile::*Uses);

/// Writes the merged section to \p Out.
void writeStrings(ArrayRef<ObjectFile> Objects, UsedStrings ObjectFile::*Uses,
                  char *Out);

} // namespace dsymutil2
} // namespace llvm

#endif
