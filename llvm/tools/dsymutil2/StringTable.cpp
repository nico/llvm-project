//===- StringTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "StringTable.h"
#include "dsymutil2.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/xxhash.h"
#include <atomic>

using namespace llvm;
using namespace llvm::dsymutil2;

namespace {
/// Identifies a use of a string: the object file and the index of the string
/// in its StringOffsets. Packed into a table cell together with a few bits of
/// the string's hash; zero is the empty cell. Among the uses of one string,
/// the smallest cell value is that of the first object file.
constexpr unsigned IndexBits = 24;
constexpr unsigned ObjectBits = 24;
constexpr uint64_t UseMask = (uint64_t(1) << (IndexBits + ObjectBits)) - 1;

uint64_t makeCell(uint64_t Hash, size_t Object, size_t Index) {
  return (Hash >> (IndexBits + ObjectBits) << (IndexBits + ObjectBits)) |
         (uint64_t(Object + 1) << IndexBits) | Index;
}
size_t objectOf(uint64_t Cell) {
  return ((Cell & UseMask) >> IndexBits) - 1;
}
size_t indexOf(uint64_t Cell) {
  return Cell & ((uint64_t(1) << IndexBits) - 1);
}

/// An open-addressing hash set of strings that threads insert into
/// concurrently. A cell is claimed for a string once and then only ever
/// changes to an earlier use of the same string.
class ConcurrentStringSet {
public:
  ConcurrentStringSet(ArrayRef<ObjectFile> Objects, size_t NumUses)
      : Objects(Objects),
        Cells(llvm::bit_ceil(std::max<size_t>(NumUses + NumUses / 2, 16))) {}

  StringRef string(size_t Object, size_t Index) const {
    return Objects[Object].string(Objects[Object].StringOffsets[Index]);
  }

  /// Inserts the given use of a string and returns the cell of the string.
  uint32_t insert(size_t Object, size_t Index) {
    StringRef Str = string(Object, Index);
    uint64_t Hash = xxh3_64bits(Str);
    uint64_t Mine = makeCell(Hash, Object, Index);
    size_t Mask = Cells.size() - 1;
    for (size_t I = Hash & Mask;; I = (I + 1) & Mask) {
      uint64_t Current = Cells[I].load(std::memory_order_relaxed);
      if (Current == 0 && Cells[I].compare_exchange_strong(Current, Mine))
        return I;
      // The cell is taken, possibly since a moment ago. Is it this string?
      if ((Current ^ Mine) & ~UseMask)
        continue;
      if (string(objectOf(Current), indexOf(Current)) != Str)
        continue;
      while (Mine < Current &&
             !Cells[I].compare_exchange_weak(Current, Mine))
        ;
      return I;
    }
  }

  uint64_t cell(uint32_t I) const {
    return Cells[I].load(std::memory_order_relaxed);
  }

private:
  ArrayRef<ObjectFile> Objects;
  std::vector<std::atomic<uint64_t>> Cells;
};
} // namespace

uint64_t dsymutil2::mergeStrings(MutableArrayRef<ObjectFile> Objects) {
  size_t NumUses = 0;
  for (const ObjectFile &Object : Objects) {
    NumUses += Object.StringOffsets.size();
    if (Object.StringOffsets.size() >= (size_t(1) << IndexBits))
      fatal("too many strings in one object file");
  }
  if (Objects.size() >= (size_t(1) << ObjectBits) - 1 || NumUses > UINT32_MAX / 2)
    fatal("too many object files or strings");
  ConcurrentStringSet Set(Objects, NumUses);

  // Find the first use of every string.
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    Object.StringCells.resize(Object.StringOffsets.size());
    for (size_t J = 0; J != Object.StringOffsets.size(); ++J)
      Object.StringCells[J] = Set.insert(I, J);
  });

  // Lay out each object's owned strings, relative to the object's block.
  auto IsOwned = [&](size_t Object, size_t Index) {
    uint64_t Cell = Set.cell(Objects[Object].StringCells[Index]);
    return objectOf(Cell) == Object && indexOf(Cell) == Index;
  };
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    Object.StringOutOffsets.resize(Object.StringOffsets.size());
    uint64_t Size = 0;
    for (size_t J = 0; J != Object.StringOffsets.size(); ++J) {
      if (!IsOwned(I, J))
        continue;
      Object.StringOutOffsets[J] = Size;
      Size += Set.string(I, J).size() + 1;
    }
    Object.StrSize = Size;
  });
  // The section starts with an empty string, so that no string that the
  // objects use is at offset 0: in an accelerator table, that offset ends a
  // list of names.
  uint64_t TotalSize = 1;
  for (ObjectFile &Object : Objects) {
    Object.StrOffset = TotalSize;
    TotalSize += Object.StrSize;
  }
  // Offsets into __debug_str are 32 bits wide in DWARF32.
  if (TotalSize > UINT32_MAX)
    fatal("the strings of the debug info do not fit in 4 GB");

  // Resolve the uses that do not own their string, which reads the owners'
  // block-relative offsets; then make the owners' offsets absolute too.
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    for (size_t J = 0; J != Object.StringOffsets.size(); ++J) {
      uint64_t Cell = Set.cell(Object.StringCells[J]);
      size_t Owner = objectOf(Cell);
      if (Owner != I || indexOf(Cell) != J)
        Object.StringOutOffsets[J] =
            Objects[Owner].StrOffset +
            Objects[Owner].StringOutOffsets[indexOf(Cell)];
    }
  });
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    for (size_t J = 0; J != Object.StringOffsets.size(); ++J) {
      bool Owned = IsOwned(I, J);
      // Remember which strings this object writes; the cells are not needed
      // any longer.
      Object.StringCells[J] = Owned;
      if (Owned)
        Object.StringOutOffsets[J] += Object.StrOffset;
    }
  });
  return TotalSize;
}

void dsymutil2::writeStrings(ArrayRef<ObjectFile> Objects, char *Out) {
  Out[0] = '\0';
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    for (size_t J = 0; J != Object.StringOffsets.size(); ++J) {
      if (!Object.StringCells[J])
        continue;
      StringRef Str = Object.string(Object.StringOffsets[J]);
      char *P = Out + Object.StringOutOffsets[J];
      memcpy(P, Str.data(), Str.size());
      P[Str.size()] = '\0';
    }
  });
}
