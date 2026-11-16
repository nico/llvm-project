//===- AccelTable.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AccelTable.h"
#include "dsymutil2.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/bit.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
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
/// list, once that is grouped by name.
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
/// of a function. So the entries of each object's list are grouped by name
/// first, and what is sorted across object files is those groups.
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
    // Number the names in the order in which they first occur, count the
    // entries of each, and move every entry to where its name's entries go.
    // This keeps the entries of a name in their order.
    std::vector<AccelT> &Accels = Objects[I].*List;
    DenseMap<uint32_t, uint32_t> NumberOfName;
    std::vector<uint32_t> Numbers(Accels.size());
    std::vector<uint32_t> Next;
    for (size_t J = 0; J != Accels.size(); ++J) {
      auto [It, Inserted] =
          NumberOfName.try_emplace(Accels[J].Name, Next.size());
      if (Inserted)
        Next.push_back(0);
      Numbers[J] = It->second;
      ++Next[It->second];
    }
    uint32_t First = 0;
    for (uint32_t &N : Next)
      First += std::exchange(N, First);
    std::vector<AccelT> Grouped(Accels.size());
    for (size_t J = 0; J != Accels.size(); ++J)
      Grouped[Next[Numbers[J]]++] = Accels[J];
    Accels = std::move(Grouped);
    FirstGroup[I + 1] = Next.size();
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
              Object.Strings.outOffset(Name), uint32_t(I), First,
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

namespace {
/// An entry of __debug_names.
struct IndexEntry {
  /// The hash of the name, with its bits in reverse order.
  uint32_t ReversedHash;
  /// The offset of the name in the output's __debug_str.
  uint32_t Name;
  /// The number of the entry's unit in the output, and where the entry is
  /// in the unit.
  uint32_t Unit;
  uint32_t Offset;
  uint16_t Tag;

  bool operator<(const IndexEntry &Other) const {
    return std::tie(ReversedHash, Name, Unit, Offset, Tag) <
           std::tie(Other.ReversedHash, Other.Name, Other.Unit, Other.Offset,
                    Other.Tag);
  }
  bool hasNameOf(const IndexEntry &Other) const {
    return ReversedHash == Other.ReversedHash && Name == Other.Name;
  }
};
} // namespace

/// The table is, in this order: a header; the offsets of the units; the
/// buckets, each the number of its first name, counted from 1; for each
/// name its hash, its offset in __debug_str and the offset of its entries;
/// the abbreviations, which say what an entry is made of; and the entries
/// of each name, ended by a zero.
///
/// As in the tables above, the names are in the order of their hashes read
/// from the lowest bit up, which puts the names of a bucket next to each
/// other for any number of buckets that is a power of two.
///
/// An entry has its unit and its offset there. It could also name the entry
/// that it is in, for a debugger to tell which "S::f" it is without reading
/// the debug info. This does not: debuggers do without.
SmallVector<char, 0>
dsymutil2::buildDebugNames(MutableArrayRef<ObjectFile> Objects) {
  std::vector<uint32_t> FirstUnit(Objects.size() + 1);
  std::vector<size_t> FirstEntry(Objects.size() + 1);
  for (size_t I = 0; I != Objects.size(); ++I) {
    const ObjectFile &Object = Objects[I];
    FirstUnit[I + 1] = FirstUnit[I] + Object.Units.size();
    FirstEntry[I + 1] = FirstEntry[I] + Object.Names.size() +
                        Object.Namespaces.size() + Object.Types.size();
  }
  uint32_t NumUnits = FirstUnit.back();
  size_t NumEntries = FirstEntry.back();
  // Not a vector: that would first fill the memory with zeros, on one thread.
  std::unique_ptr<IndexEntry[]> Entries(new IndexEntry[NumEntries]);
  parallelFor(0, Objects.size(), [&](size_t I) {
    ObjectFile &Object = Objects[I];
    IndexEntry *E = Entries.get() + FirstEntry[I];
    auto Add = [&](uint32_t Name, uint32_t Entry, uint16_t Tag) {
      auto Unit = std::prev(llvm::upper_bound(
          Object.Units, Entry, [](uint32_t Entry, const UnitInfo &Unit) {
            return Entry < Unit.OutOffset;
          }));
      *E++ = {llvm::reverseBits(caseFoldingDjbHash(Object.string(Name))),
              Object.Strings.outOffset(Name),
              FirstUnit[I] + uint32_t(Unit - Object.Units.begin()),
              Entry - Unit->OutOffset, Tag};
    };
    for (size_t J = 0; J != Object.Names.size(); ++J)
      Add(Object.Names[J].Name, Object.Names[J].Entry, Object.NameTags[J]);
    for (size_t J = 0; J != Object.Namespaces.size(); ++J)
      Add(Object.Namespaces[J].Name, Object.Namespaces[J].Entry,
          Object.NamespaceTags[J]);
    for (const AccelType &Type : Object.Types)
      Add(Type.Name, Type.Entry, Type.Tag);
    std::vector<AccelName>().swap(Object.Names);
    std::vector<AccelName>().swap(Object.Namespaces);
    std::vector<AccelName>().swap(Object.ObjCClasses);
    std::vector<AccelType>().swap(Object.Types);
  });
  parallelSort(Entries.get(), Entries.get() + NumEntries);

  // An abbreviation per tag: its number, the tag, two pairs of what a value
  // is and its form, and a pair of zeros. A zero ends them.
  unsigned UnitSize = NumUnits <= 0x100 ? 1 : NumUnits <= 0x10000 ? 2 : 4;
  unsigned UnitForm = UnitSize == 1   ? dwarf::DW_FORM_data1
                      : UnitSize == 2 ? dwarf::DW_FORM_data2
                                      : dwarf::DW_FORM_data4;
  unsigned EntrySize = 1 + UnitSize + 4;
  std::vector<uint16_t> Tags;
  // Where the entries of each name start, and those of each hash.
  std::vector<size_t> Names;
  size_t NumHashes = 0;
  for (size_t I = 0; I != NumEntries; ++I) {
    if (I == 0 || !Entries[I].hasNameOf(Entries[I - 1]))
      Names.push_back(I);
    NumHashes +=
        I == 0 || Entries[I].ReversedHash != Entries[I - 1].ReversedHash;
    if (!llvm::is_contained(Tags, Entries[I].Tag))
      Tags.push_back(Entries[I].Tag);
  }
  llvm::sort(Tags);
  if (Tags.size() >= 0x80)
    fatal("too many kinds of entries for __debug_names");
  SmallVector<uint8_t, 256> Abbreviations;
  for (size_t I = 0; I != Tags.size(); ++I) {
    uint8_t TagBytes[3];
    unsigned TagSize = encodeULEB128(Tags[I], TagBytes);
    Abbreviations.push_back(I + 1);
    Abbreviations.append(TagBytes, TagBytes + TagSize);
    Abbreviations.append({dwarf::DW_IDX_compile_unit, uint8_t(UnitForm),
                          dwarf::DW_IDX_die_offset, dwarf::DW_FORM_ref4, 0, 0});
  }
  Abbreviations.push_back(0);
  Names.push_back(NumEntries);
  size_t NumNames = Names.size() - 1;

  // About four hashes per bucket, as in the tables above.
  uint64_t NumBuckets = llvm::bit_ceil(std::max<uint64_t>(NumHashes / 4, 1));
  uint64_t UnitsOffset = 44;
  uint64_t BucketsOffset = UnitsOffset + 4 * uint64_t(NumUnits);
  uint64_t HashesOffset = BucketsOffset + 4 * NumBuckets;
  uint64_t StringsOffset = HashesOffset + 4 * NumNames;
  uint64_t EntryOffsetsOffset = StringsOffset + 4 * NumNames;
  uint64_t AbbreviationsOffset = EntryOffsetsOffset + 4 * NumNames;
  uint64_t PoolOffset = AbbreviationsOffset + Abbreviations.size();
  uint64_t Size = PoolOffset + uint64_t(NumEntries) * EntrySize + NumNames;
  // The table has offsets into itself that are 32 bits wide.
  if (Size > UINT32_MAX)
    fatal("__debug_names does not fit in 4 GB");

  SmallVector<char, 0> Table;
  Table.resize_for_overwrite(Size);
  uint8_t *Out = reinterpret_cast<uint8_t *>(Table.data());
  write32le(Out, Size - 4);
  write16le(Out + 4, 5); // version
  write16le(Out + 6, 0); // padding
  write32le(Out + 8, NumUnits);
  write32le(Out + 12, 0); // type units
  write32le(Out + 16, 0); // type units of other files
  write32le(Out + 20, NumBuckets);
  write32le(Out + 24, NumNames);
  write32le(Out + 28, Abbreviations.size());
  // Who made the table, as LLVM says it.
  write32le(Out + 32, 8);
  memcpy(Out + 36, "LLVM0700", 8);
  parallelFor(0, Objects.size(), [&](size_t I) {
    uint8_t *P = Out + UnitsOffset + 4 * uint64_t(FirstUnit[I]);
    for (const UnitInfo &Unit : Objects[I].Units) {
      write32le(P, Objects[I].InfoOffset + Unit.OutOffset);
      P += 4;
    }
  });
  memset(Out + BucketsOffset, 0, 4 * NumBuckets);
  memcpy(Out + AbbreviationsOffset, Abbreviations.data(), Abbreviations.size());

  auto Bucket = [&](size_t Name) {
    return llvm::reverseBits(Entries[Names[Name]].ReversedHash) &
           (NumBuckets - 1);
  };
  parallelFor(0, NumNames, [&](size_t N) {
    const IndexEntry &First = Entries[Names[N]];
    if (N == 0 || Bucket(N - 1) != Bucket(N))
      write32le(Out + BucketsOffset + 4 * Bucket(N), N + 1);
    write32le(Out + HashesOffset + 4 * N,
              llvm::reverseBits(First.ReversedHash));
    write32le(Out + StringsOffset + 4 * N, First.Name);
    // The entries of the names before this one, and a zero for each.
    uint64_t Offset = uint64_t(Names[N]) * EntrySize + N;
    write32le(Out + EntryOffsetsOffset + 4 * N, Offset);
    uint8_t *P = Out + PoolOffset + Offset;
    for (size_t I = Names[N]; I != Names[N + 1]; ++I) {
      const IndexEntry &E = Entries[I];
      *P++ = llvm::lower_bound(Tags, E.Tag) - Tags.begin() + 1;
      if (UnitSize == 1)
        *P = E.Unit;
      else if (UnitSize == 2)
        write16le(P, E.Unit);
      else
        write32le(P, E.Unit);
      write32le(P + UnitSize, E.Offset);
      P += UnitSize + 4;
    }
    *P = 0;
  });
  return Table;
}
