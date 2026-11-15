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

/// An element of a table: a name and an entry that has it, as they are in the
/// output.
struct NameRecord {
  /// The hash of the name, with its bits in reverse order.
  uint32_t ReversedHash;
  /// The offset of the name in __debug_str.
  uint32_t Name;
  /// The offset of the entry in __debug_info.
  uint32_t Entry;

  NameRecord() = default;
  NameRecord(const ObjectFile &Object, const AccelName &Accel)
      : ReversedHash(llvm::reverseBits(djbHash(Object.string(Accel.Name)))),
        Name(Object.stringOutOffset(Accel.Name)),
        Entry(Object.InfoOffset + Accel.Entry) {}

  bool operator<(const NameRecord &Other) const {
    return std::tie(ReversedHash, Name, Entry) <
           std::tie(Other.ReversedHash, Other.Name, Other.Entry);
  }

  static constexpr Atom Atoms[] = {
      {dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4}};
  static constexpr unsigned EntrySize = 4;
  void writeEntry(uint8_t *P) const { write32le(P, Entry); }
};

struct TypeRecord : NameRecord {
  uint32_t QualifiedNameHash;
  uint16_t Tag;
  uint8_t Flags;

  TypeRecord() = default;
  TypeRecord(const ObjectFile &Object, const AccelType &Accel)
      : NameRecord(Object, {Accel.Name, Accel.Entry}),
        QualifiedNameHash(Accel.QualifiedNameHash), Tag(Accel.Tag),
        Flags(Accel.IsObjCImplementation ? dwarf::DW_FLAG_type_implementation
                                         : 0) {}

  static constexpr Atom Atoms[] = {
      {dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4},
      {dwarf::DW_ATOM_die_tag, dwarf::DW_FORM_data2},
      {dwarf::DW_ATOM_type_type_flags, dwarf::DW_FORM_data1},
      {dwarf::DW_ATOM_qual_name_hash, dwarf::DW_FORM_data4}};
  static constexpr unsigned EntrySize = 11;
  void writeEntry(uint8_t *P) const {
    write32le(P, Entry);
    write16le(P + 4, Tag);
    P[6] = Flags;
    write32le(P + 7, QualifiedNameHash);
  }
};

/// A run of records that one thread turns into a part of the table.
struct Chunk {
  /// The first record.
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
/// The records are sorted by their hashes read from the lowest bit up. The
/// number of buckets is a power of two, so a bucket is the low bits of a
/// hash, and that order puts the records of a bucket next to each other, for
/// any number of buckets: the number can be chosen after the sort, when the
/// number of distinct hashes is known. The buckets are then not in the order
/// of their indexes in the table. Nothing needs them to be.
template <class RecordT, class AccelT>
static SmallVector<char, 0> build(MutableArrayRef<ObjectFile> Objects,
                                  std::vector<AccelT> ObjectFile::*List) {
  std::vector<size_t> FirstRecord(Objects.size() + 1);
  for (size_t I = 0; I != Objects.size(); ++I)
    FirstRecord[I + 1] = FirstRecord[I] + (Objects[I].*List).size();
  size_t NumRecords = FirstRecord.back();
  // Not a vector: that would first fill the memory with zeros, on one thread.
  std::unique_ptr<RecordT[]> Records(new RecordT[NumRecords]);
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    RecordT *Record = Records.get() + FirstRecord[I];
    for (const AccelT &Accel : Object.*List)
      *Record++ = RecordT(Object, Accel);
    std::vector<AccelT>().swap(Object.*List);
  });
  parallelSort(Records.get(), Records.get() + NumRecords);

  // Chunks of the records are written in parallel. A chunk starts with the
  // first record of a hash.
  constexpr size_t ChunkSize = 1 << 14;
  size_t NumChunks = divideCeil(NumRecords, ChunkSize);
  std::vector<Chunk> Chunks(NumChunks + 1);
  parallelFor(0, NumChunks + 1, [&](size_t C) {
    size_t I = std::min(C * ChunkSize, NumRecords);
    while (I != 0 && I != NumRecords &&
           Records[I].ReversedHash == Records[I - 1].ReversedHash)
      ++I;
    Chunks[C].Begin = I;
  });
  parallelFor(0, NumChunks, [&](size_t C) {
    Chunk &Chunk = Chunks[C];
    for (size_t I = Chunk.Begin, End = Chunks[C + 1].Begin; I != End; ++I) {
      bool IsNewHash = I == Chunk.Begin ||
                       Records[I].ReversedHash != Records[I - 1].ReversedHash;
      bool IsNewName = IsNewHash || Records[I].Name != Records[I - 1].Name;
      Chunk.NumHashes += IsNewHash;
      // A name has its offset and the number of its entries. A zero ends
      // the names of a hash.
      Chunk.DataSize +=
          RecordT::EntrySize + (IsNewName ? 8 : 0) + (IsNewHash ? 4 : 0);
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
  uint64_t HeaderDataSize = 8 + 4 * std::size(RecordT::Atoms);
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
  write32le(Out + 24, std::size(RecordT::Atoms));
  uint8_t *P = Out + 28;
  for (Atom A : RecordT::Atoms) {
    write16le(P, A.Type);
    write16le(P + 2, A.Form);
    P += 4;
  }
  // An empty bucket has this.
  memset(Out + BucketsOffset, 0xff, 4 * NumBuckets);

  auto Bucket = [&](const RecordT &Record) {
    return llvm::reverseBits(Record.ReversedHash) & (NumBuckets - 1);
  };
  parallelFor(0, NumChunks, [&](size_t C) {
    uint64_t HashIndex = Chunks[C].FirstHash;
    uint8_t *P = Out + DataOffset + Chunks[C].DataOffset;
    for (size_t I = Chunks[C].Begin, End = Chunks[C + 1].Begin; I != End;) {
      uint32_t ReversedHash = Records[I].ReversedHash;
      if (I == 0 || Bucket(Records[I - 1]) != Bucket(Records[I]))
        write32le(Out + BucketsOffset + 4 * Bucket(Records[I]), HashIndex);
      write32le(Out + HashesOffset + 4 * HashIndex,
                llvm::reverseBits(ReversedHash));
      write32le(Out + OffsetsOffset + 4 * HashIndex, P - Out);
      ++HashIndex;
      while (I != End && Records[I].ReversedHash == ReversedHash) {
        uint32_t Name = Records[I].Name;
        uint8_t *NumEntries = P + 4;
        write32le(P, Name);
        P += 8;
        size_t First = I;
        for (; I != End && Records[I].ReversedHash == ReversedHash &&
               Records[I].Name == Name;
             ++I) {
          Records[I].writeEntry(P);
          P += RecordT::EntrySize;
        }
        write32le(NumEntries, I - First);
      }
      write32le(P, 0);
      P += 4;
    }
  });
  return Table;
}

SmallVector<char, 0>
dsymutil2::buildNameTable(MutableArrayRef<ObjectFile> Objects,
                          std::vector<AccelName> ObjectFile::*Names) {
  return build<NameRecord>(Objects, Names);
}

SmallVector<char, 0>
dsymutil2::buildTypeTable(MutableArrayRef<ObjectFile> Objects) {
  return build<TypeRecord>(Objects, &ObjectFile::Types);
}
