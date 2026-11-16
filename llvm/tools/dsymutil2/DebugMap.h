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
#include <cstdint>
#include <vector>

namespace llvm {
namespace dsymutil2 {
class MachOFile;

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
  /// When the file was last changed, in seconds, as the linker saw it. An
  /// archive can have two members with the same name: this tells them
  /// apart.
  uint64_t Timestamp = 0;
  std::vector<MapSymbol> Symbols;
};

struct DebugMap {
  std::vector<MapObject> Objects;
  /// The files of the Swift modules that the linker was told of, each once.
  std::vector<StringRef> SwiftModules;
  /// The addresses of all symbols defined in the binary, sorted. No symbol
  /// of the debug map extends past the next one of these.
  std::vector<uint64_t> SymbolAddresses;
};

/// Reads the debug map of \p Binary. The strings of the result point into the
/// file.
DebugMap readDebugMap(const MachOFile &Binary);

} // namespace dsymutil2
} // namespace llvm

#endif
