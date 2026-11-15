//===- DebugMap.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DebugMap.h"
#include "MachOFile.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/MachO.h"

using namespace llvm;
using namespace llvm::dsymutil2;

DebugMap dsymutil2::readDebugMap(const MachOFile &Binary) {
  // Addresses of the binary's regular symbols by name, to resolve N_GSYM
  // stabs, which carry no address. Built on first use.
  DenseMap<StringRef, uint64_t> GlobalAddresses;
  bool HaveGlobalAddresses = false;
  auto AddressOfGlobal = [&](StringRef Name) {
    if (!HaveGlobalAddresses) {
      HaveGlobalAddresses = true;
      for (uint32_t I = 0; I != Binary.NumSymbols; ++I) {
        MachOSymbol S = Binary.symbol(I);
        if (!S.isDefinedInSection())
          continue;
        // Globals are external or, once the linker has hidden them, local
        // symbols in data sections. An external symbol wins over a local one
        // of the same name.
        bool IsCode =
            S.Section != 0 && S.Section <= Binary.Sections.size() &&
            (Binary.Sections[S.Section - 1].Flags &
             (MachO::S_ATTR_PURE_INSTRUCTIONS |
              MachO::S_ATTR_SOME_INSTRUCTIONS));
        if (S.Type & MachO::N_EXT)
          GlobalAddresses[S.Name] = S.Value;
        else if (!IsCode)
          GlobalAddresses.try_emplace(S.Name, S.Value);
      }
    }
    return GlobalAddresses.lookup(Name);
  };

  DebugMap Map;
  MapObject *Object = nullptr;
  for (uint32_t I = 0; I != Binary.NumSymbols; ++I) {
    MachOSymbol S = Binary.symbol(I);
    if (S.Type == MachO::N_OSO) {
      Object = &Map.Objects.emplace_back();
      Object->Path = S.Name;
      continue;
    }
    if (!Object)
      continue;
    switch (S.Type) {
    case MachO::N_SO:
      // The end of the stabs of one object file.
      if (S.Name.empty())
        Object = nullptr;
      break;
    case MachO::N_FUN:
      // A function has two entries: its name and address, then an entry
      // without a name that holds its size.
      if (!S.Name.empty())
        Object->Symbols.push_back({S.Name, S.Value, 0, MapSymbol::Function});
      else if (!Object->Symbols.empty() &&
               Object->Symbols.back().Kind == MapSymbol::Function)
        Object->Symbols.back().Size = S.Value;
      break;
    case MachO::N_GSYM:
      if (uint64_t Address = AddressOfGlobal(S.Name))
        Object->Symbols.push_back({S.Name, Address, 0, MapSymbol::Global});
      break;
    case MachO::N_STSYM:
      Object->Symbols.push_back({S.Name, S.Value, 0, MapSymbol::Static});
      break;
    }
  }
  return Map;
}
