//===- AccelTable.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The Apple accelerator tables: hash tables from a name to the entries of
// __debug_info that have it. With them, a debugger finds a function or a
// type by name without reading the debug info of every unit first.
//
// A table is a header, an array of buckets, an array of the distinct hashes
// of the names, an array that has the offset of the data of each of those,
// and the data. The names with one hash follow each other in the data: for
// each, the name's offset in __debug_str, the number of its entries, and the
// entries. A zero ends them. A bucket has the index of its first hash in the
// hash array; its other hashes follow that one.
//
// The scan collects the names per object file. Here they are gathered and
// sorted by hash, which depends on nothing but the name: the output is the
// same no matter how the work was spread over threads.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_ACCELTABLE_H
#define LLVM_TOOLS_DSYMUTIL2_ACCELTABLE_H

#include "ObjectFile.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
namespace dsymutil2 {

/// Returns the contents of the table of the names in the list \p Names of
/// all of \p Objects. Needs the layout of __debug_info and __debug_str, and
/// empties the lists.
SmallVector<char, 0> buildNameTable(MutableArrayRef<ObjectFile> Objects,
                                    std::vector<AccelName> ObjectFile::*Names);

/// The same for the table of types, from ObjectFile::Types.
SmallVector<char, 0> buildTypeTable(MutableArrayRef<ObjectFile> Objects);

} // namespace dsymutil2
} // namespace llvm

#endif
