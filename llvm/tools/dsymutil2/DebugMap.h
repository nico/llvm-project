//===- DebugMap.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The debug map is the list of object files a Mach-O binary was linked from,
// and for each of them the symbols that made it into the binary with their
// addresses there. The linker records it in the binary's symbol table as
// stabs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_DEBUGMAP_H
#define LLVM_TOOLS_DSYMUTIL2_DEBUGMAP_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <cstdint>
#include <vector>

namespace llvm {
class raw_ostream;

namespace dsymutil2 {

struct MapSymbol {
  enum KindTy : uint8_t { Function, Global, Static };

  StringRef Name;
  /// Address in the linked binary.
  uint64_t Address;
  /// Only known for functions.
  uint64_t Size;
  KindTy Kind;
};

struct MapObject {
  /// As recorded by the linker: "/path/to/foo.o" or "/path/to/lib.a(foo.o)".
  StringRef Path;
  std::vector<MapSymbol> Symbols;
};

struct DebugMap {
  std::vector<MapObject> Objects;
};

/// Reads the debug map of the thin 64-bit little-endian Mach-O file \p Binary.
/// The strings of the result point into \p Binary.
DebugMap readDebugMap(MemoryBufferRef Binary);

void dumpDebugMap(const DebugMap &Map, raw_ostream &OS);

} // namespace dsymutil2
} // namespace llvm

#endif
