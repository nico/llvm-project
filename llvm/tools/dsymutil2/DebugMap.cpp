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
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Parallel.h"
#include <atomic>

using namespace llvm;
using namespace llvm::dsymutil2;

DebugMap dsymutil2::readDebugMap(const MachOFile &Binary) {
  DebugMap Map;
  // The stabs of an object file start with an N_OSO. This loop looks at the
  // type of every symbol and at nothing else; the names are read by the
  // loops below, in parallel.
  std::vector<uint32_t> ObjectStabs;
  for (uint32_t I = 0; I != Binary.NumSymbols; ++I) {
    MachOSymbol S = Binary.symbolWithoutName(I);
    if (S.isDefinedInSection())
      Map.SymbolAddresses.push_back(S.Value);
    else if (S.Type == MachO::N_OSO)
      ObjectStabs.push_back(I);
  }
  parallelSort(Map.SymbolAddresses.begin(), Map.SymbolAddresses.end());
  Map.SymbolAddresses.erase(llvm::unique(Map.SymbolAddresses),
                            Map.SymbolAddresses.end());

  Map.Objects.resize(ObjectStabs.size());
  parallelFor(0, ObjectStabs.size(), [&](size_t O) {
    MapObject &Object = Map.Objects[O];
    Object.Path = Binary.symbol(ObjectStabs[O]).Name;
    uint32_t End =
        O + 1 != ObjectStabs.size() ? ObjectStabs[O + 1] : Binary.NumSymbols;
    for (uint32_t I = ObjectStabs[O] + 1; I != End; ++I) {
      if (!Binary.symbolWithoutName(I).isStab())
        continue;
      MachOSymbol S = Binary.symbol(I);
      switch (S.Type) {
      case MachO::N_SO:
        // The end of the stabs of one object file.
        if (S.Name.empty())
          return;
        break;
      case MachO::N_FUN:
        // A function has two entries: its name and address, then an entry
        // without a name that holds its size.
        if (!S.Name.empty())
          Object.Symbols.push_back({S.Name, S.Value, 0, MapSymbol::Function});
        else if (!Object.Symbols.empty() &&
                 Object.Symbols.back().Kind == MapSymbol::Function)
          Object.Symbols.back().Size = S.Value;
        break;
      case MachO::N_GSYM:
        // Has no address: see below.
        Object.Symbols.push_back({S.Name, 0, 0, MapSymbol::Global});
        break;
      case MachO::N_STSYM:
        Object.Symbols.push_back({S.Name, S.Value, 0, MapSymbol::Static});
        break;
      }
    }
  });

  // The address of an N_GSYM is that of the binary's regular symbol with its
  // name. Number the names that are looked for; until its address is known,
  // a global has the number of its name in its place.
  DenseMap<StringRef, uint32_t> GlobalNames;
  for (MapObject &Object : Map.Objects)
    for (MapSymbol &Symbol : Object.Symbols)
      if (Symbol.Kind == MapSymbol::Global)
        Symbol.Address =
            GlobalNames.try_emplace(Symbol.Name, GlobalNames.size())
                .first->second;
  // For each name, the best symbol with it so far: its index + 1, and above
  // that whether it is a local symbol. Globals are external or, once the
  // linker has hidden them, local symbols in data sections. An external
  // symbol wins over a local one, and the first symbol over later ones; so
  // the smallest value does.
  std::vector<std::atomic<uint64_t>> Best(GlobalNames.size());
  constexpr uint32_t RunSize = 1 << 15;
  parallelFor(0, divideCeil(Binary.NumSymbols, RunSize), [&](size_t R) {
    uint32_t End =
        std::min<uint64_t>(uint64_t(R + 1) * RunSize, Binary.NumSymbols);
    for (uint32_t I = R * RunSize; I != End; ++I) {
      MachOSymbol S = Binary.symbolWithoutName(I);
      if (!S.isDefinedInSection())
        continue;
      bool IsLocal = !(S.Type & MachO::N_EXT);
      bool IsCode =
          S.Section != 0 && S.Section <= Binary.Sections.size() &&
          (Binary.Sections[S.Section - 1].Flags &
           (MachO::S_ATTR_PURE_INSTRUCTIONS | MachO::S_ATTR_SOME_INSTRUCTIONS));
      if (IsLocal && IsCode)
        continue;
      auto It = GlobalNames.find(Binary.symbol(I).Name);
      if (It == GlobalNames.end())
        continue;
      uint64_t New = uint64_t(IsLocal) << 32 | (uint64_t(I) + 1);
      std::atomic<uint64_t> &Cell = Best[It->second];
      uint64_t Old = Cell.load(std::memory_order_relaxed);
      while ((Old == 0 || New < Old) && !Cell.compare_exchange_weak(Old, New))
        ;
    }
  });
  parallelFor(0, Map.Objects.size(), [&](size_t O) {
    std::vector<MapSymbol> &Symbols = Map.Objects[O].Symbols;
    for (MapSymbol &Symbol : Symbols) {
      if (Symbol.Kind != MapSymbol::Global)
        continue;
      uint32_t Index = Best[Symbol.Address].load(std::memory_order_relaxed);
      Symbol.Address = Index ? Binary.symbolWithoutName(Index - 1).Value : 0;
    }
    // A global that the binary does not have is not in the debug map.
    llvm::erase_if(Symbols, [](const MapSymbol &Symbol) {
      return Symbol.Kind == MapSymbol::Global && Symbol.Address == 0;
    });
  });
  return Map;
}
