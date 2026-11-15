//===- dsymutil2.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_DSYMUTIL2_H
#define LLVM_TOOLS_DSYMUTIL2_DSYMUTIL2_H

#include "llvm/ADT/Twine.h"

namespace llvm {
namespace dsymutil2 {

/// Prints an error and exits.
[[noreturn]] void fatal(const Twine &Message);

/// Prints a warning. Not thread-safe: parallel passes record their warnings
/// and the driver prints them afterwards, in a fixed order.
void warn(const Twine &Message);

} // namespace dsymutil2
} // namespace llvm

#endif
