//===- AccelTable.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AccelTable.h"
#include "dsymutil2.h"
#include "llvm/ADT/bit.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Parallel.h"
#include <tuple>

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

namespace {
/// What a table says about each of its entries: a kind of value, and its
/// form.
struct Atom {
  uint16_t Type;
  uint16_t Form;
};

/// How the tables of names write an entry.
struct NameTable {
  static constexpr Atom Atoms[] = {
      {dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4}};
  static constexpr unsigned EntrySize = 4;
  static void writeEntry(uint8_t *P, const ObjectFile &Object,
                         const AccelName &Accel) {
    write32le(P, Object.InfoOffset + Accel.Entry);
  }
};

/// How the table of types does.
struct TypeTable {
  static constexpr Atom Atoms[] = {
      {dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4},
      {dwarf::DW_ATOM_die_tag, dwarf::DW_FORM_data2},
      {dwarf::DW_ATOM_type_type_flags, dwarf::DW_FORM_data1},
      {dwarf::DW_ATOM_qual_name_hash, dwarf::DW_FORM_data4}};
  static constexpr unsigned EntrySize = 11;
  static void writeEntry(uint8_t *P, const ObjectFile &Object,
                         const AccelType &Accel) {
    write32le(P, Object.InfoOffset + Accel.Entry);
    write16le(P + 4, Accel.Tag);
    P[6] = Accel.IsObjCImplementation ? dwarf::DW_FLAG_type_implementation : 0;
    write32le(P + 7, Accel.QualifiedNameHash);
  }
};

/// The entries that one object file has for one name: a run of the object's
/// list, once that is sorted by name.
struct Group {
  /// The hash of the name, with its bits in reverse order.
  uint32_t ReversedHash;
  /// The offset of the name in the output's __debug_str.
  uint32_t Name;
  uint32_t Object;
  uint32_t First;
  uint32_t Count;

  bool operator<(const Group &Other) const {
    return std::tie(ReversedHash, Name, Object, First) <
           std::tie(Other.ReversedHash, Other.Name, Other.Object, Other.First);
  }
};

/// A run of groups that one thread turns into a part of the table.
struct Chunk {
  /// The first group.
  size_t Begin;
  /// The number of distinct hashes in the chunk and the size of its data.
  uint64_t NumHashes = 0;
  uint64_t DataSize = 0;
  /// The same, summed over the chunks before this one.
  uint64_t FirstHash = 0;
  uint64_t DataOffset = 0;
};
} // namespace

/// Builds the table of the names in the list \p List of all object files.
///
/// An object file often has many entries with one name, the inlined copies
/// of a function. So each object's list is sorted by name first, and what is
/// sorted across object files is the groups of entries with one name.
///
/// The groups are sorted by their hashes read from the lowest bit up. The
/// number of buckets is a power of two, so a bucket is the low bits of a
/// hash, and that order puts the groups of a bucket next to each other, for
/// any number of buckets: the number can be chosen after the sort, when the
/// number of distinct hashes is known. The buckets are then not in the order
/// of their indexes in the table. Nothing needs them to be.
template <class TableT, class AccelT>
static SmallVector<char, 0> build(MutableArrayRef<ObjectFile> Objects,
                                  std::vector<AccelT> ObjectFile::*List) {
  std::vector<size_t> FirstGroup(Objects.size() + 1);
  parallelFor(0, Objects.size(), [&](size_t I) {
    std::vector<AccelT> &Accels = Objects[I].*List;
    llvm::sort(Accels, [](const AccelT &A, const AccelT &B) {
      return std::tie(A.Name, A.Entry) < std::tie(B.Name, B.Entry);
    });
    size_t NumNames = 0;
    for (size_t J = 0; J != Accels.size(); ++J)
      NumNames += J == 0 || Accels[J].Name != Accels[J - 1].Name;
    FirstGroup[I + 1] = NumNames;
  });
  for (size_t I = 0; I != Objects.size(); ++I)
    FirstGroup[I + 1] += FirstGroup[I];
  size_t NumGroups = FirstGroup.back();
  // Not a vector: that would first fill the memory with zeros, on one thread.
  std::unique_ptr<Group[]> Groups(new Group[NumGroups]);
  parallelFor(0, Objects.size(), [&](size_t I) {
    const ObjectFile &Object = Objects[I];
    ArrayRef<AccelT> Accels = Object.*List;
    Group *G = Groups.get() + FirstGroup[I];
    for (size_t J = 0; J != Accels.size();) {
      uint32_t First = J;
      uint32_t Name = Accels[J].Name;
      while (J != Accels.size() && Accels[J].Name == Name)
        ++J;
      *G++ = {llvm::reverseBits(djbHash(Object.string(Name))),
              Object.stringOutOffset(Name), uint32_t(I), First,
              uint32_t(J - First)};
    }
  });
  parallelSort(Groups.get(), Groups.get() + NumGroups);

  // Chunks of the groups are written in parallel. A chunk starts with the
  // first group of a hash.
  constexpr size_t ChunkSize = 1 << 12;
  size_t NumChunks = divideCeil(NumGroups, ChunkSize);
  std::vector<Chunk> Chunks(NumChunks + 1);
  parallelFor(0, NumChunks + 1, [&](size_t C) {
    size_t I = std::min(C * ChunkSize, NumGroups);
    while (I != 0 && I != NumGroups &&
           Groups[I].ReversedHash == Groups[I - 1].ReversedHash)
      ++I;
    Chunks[C].Begin = I;
  });
  parallelFor(0, NumChunks, [&](size_t C) {
    Chunk &Chunk = Chunks[C];
    for (size_t I = Chunk.Begin, End = Chunks[C + 1].Begin; I != End; ++I) {
      bool IsNewHash = I == Chunk.Begin ||
                       Groups[I].ReversedHash != Groups[I - 1].ReversedHash;
      bool IsNewName = IsNewHash || Groups[I].Name != Groups[I - 1].Name;
      Chunk.NumHashes += IsNewHash;
      // A name has its offset and the number of its entries. A zero ends
      // the names of a hash.
      Chunk.DataSize += uint64_t(Groups[I].Count) * TableT::EntrySize +
                        (IsNewName ? 8 : 0) + (IsNewHash ? 4 : 0);
    }
  });
  uint64_t NumHashes = 0;
  uint64_t DataSize = 0;
  for (Chunk &Chunk : Chunks) {
    Chunk.FirstHash = NumHashes;
    Chunk.DataOffset = DataSize;
    NumHashes += Chunk.NumHashes;
    DataSize += Chunk.DataSize;
  }

  // About four hashes per bucket, like dsymutil has in its large tables.
  uint64_t NumBuckets = llvm::bit_ceil(std::max<uint64_t>(NumHashes / 4, 1));
  uint64_t HeaderDataSize = 8 + 4 * std::size(TableT::Atoms);
  uint64_t BucketsOffset = 20 + HeaderDataSize;
  uint64_t HashesOffset = BucketsOffset + 4 * NumBuckets;
  uint64_t OffsetsOffset = HashesOffset + 4 * NumHashes;
  uint64_t DataOffset = OffsetsOffset + 4 * NumHashes;
  uint64_t Size = DataOffset + DataSize;
  // The table has offsets into itself that are 32 bits wide.
  if (Size > UINT32_MAX)
    fatal("an accelerator table does not fit in 4 GB");

  SmallVector<char, 0> Table;
  Table.resize_for_overwrite(Size);
  uint8_t *Out = reinterpret_cast<uint8_t *>(Table.data());
  write32le(Out, 0x48415348); // magic: 'HASH'
  write16le(Out + 4, 1);      // version
  write16le(Out + 6, 0);      // hash function: djb
  write32le(Out + 8, NumBuckets);
  write32le(Out + 12, NumHashes);
  write32le(Out + 16, HeaderDataSize);
  write32le(Out + 20, 0); // what entry offsets are relative to
  write32le(Out + 24, std::size(TableT::Atoms));
  uint8_t *P = Out + 28;
  for (Atom A : TableT::Atoms) {
    write16le(P, A.Type);
    write16le(P + 2, A.Form);
    P += 4;
  }
  // An empty bucket has this.
  memset(Out + BucketsOffset, 0xff, 4 * NumBuckets);

  auto Bucket = [&](const Group &G) {
    return llvm::reverseBits(G.ReversedHash) & (NumBuckets - 1);
  };
  parallelFor(0, NumChunks, [&](size_t C) {
    uint64_t HashIndex = Chunks[C].FirstHash;
    uint8_t *P = Out + DataOffset + Chunks[C].DataOffset;
    for (size_t I = Chunks[C].Begin, End = Chunks[C + 1].Begin; I != End;) {
      uint32_t ReversedHash = Groups[I].ReversedHash;
      if (I == 0 || Bucket(Groups[I - 1]) != Bucket(Groups[I]))
        write32le(Out + BucketsOffset + 4 * Bucket(Groups[I]), HashIndex);
      write32le(Out + HashesOffset + 4 * HashIndex,
                llvm::reverseBits(ReversedHash));
      write32le(Out + OffsetsOffset + 4 * HashIndex, P - Out);
      ++HashIndex;
      while (I != End && Groups[I].ReversedHash == ReversedHash) {
        uint32_t Name = Groups[I].Name;
        uint8_t *NumEntries = P + 4;
        write32le(P, Name);
        P += 8;
        uint8_t *FirstEntry = P;
        for (; I != End && Groups[I].ReversedHash == ReversedHash &&
               Groups[I].Name == Name;
             ++I) {
          const ObjectFile &Object = Objects[Groups[I].Object];
          for (const AccelT &Accel :
               ArrayRef(Object.*List).slice(Groups[I].First, Groups[I].Count)) {
            TableT::writeEntry(P, Object, Accel);
            P += TableT::EntrySize;
          }
        }
        write32le(NumEntries, (P - FirstEntry) / TableT::EntrySize);
      }
      write32le(P, 0);
      P += 4;
    }
  });
  parallelFor(0, Objects.size(),
              [&](size_t I) { std::vector<AccelT>().swap(Objects[I].*List); });
  return Table;
}

SmallVector<char, 0>
dsymutil2::buildNameTable(MutableArrayRef<ObjectFile> Objects,
                          std::vector<AccelName> ObjectFile::*Names) {
  return build<NameTable>(Objects, Names);
}

SmallVector<char, 0>
dsymutil2::buildTypeTable(MutableArrayRef<ObjectFile> Objects) {
  return build<TypeTable>(Objects, &ObjectFile::Types);
}
