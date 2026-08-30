//===- ICF.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ICF is short for Identical Code Folding. That is a size optimization to
// identify and merge two or more read-only sections (typically functions)
// that happened to have the same contents. It usually reduces output size
// by a few percent.
//
// On Windows, ICF is enabled by default.
//
// See ELF/ICF.cpp for the details about the algorithm.
//
//===----------------------------------------------------------------------===//

#include "ICF.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Symbols.h"
#include "lld/Common/Timer.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <atomic>
#include <vector>

using namespace llvm;

namespace lld::coff {

class ICF {
public:
  ICF(COFFLinkerContext &c) : ctx(c){};
  void run();

private:
  // A class with more than one section: a range of `chunks`.
  using Range = std::pair<uint32_t, uint32_t>;

  void segregate(size_t begin, size_t end, bool constant,
                 std::vector<Range> &out, std::vector<SectionChunk *> &alone);
  void segregateAll(bool constant);
  std::vector<size_t> rangeBlocks() const;

  bool assocEquals(const SectionChunk *a, const SectionChunk *b);

  bool equalsConstant(const SectionChunk *a, const SectionChunk *b);
  bool equalsVariable(const SectionChunk *a, const SectionChunk *b);

  bool isEligible(SectionChunk *c);

  std::vector<SectionChunk *> chunks;
  // The classes of the current partition that could still split (a section
  // alone in its class stays so): only those are looked at in an iteration.
  std::vector<Range> ranges;
  int cnt = 0;
  std::atomic<bool> repeat = {false};

  COFFLinkerContext &ctx;
};

// Returns true if section S is subject of ICF.
//
// Microsoft's documentation
// (https://msdn.microsoft.com/en-us/library/bxwfs976.aspx; visited April
// 2017) says that /opt:icf folds both functions and read-only data.
// Despite that, the MSVC linker folds only functions. We found
// a few instances of programs that are not safe for data merging.
// Therefore, we merge only functions just like the MSVC tool. However, we also
// merge read-only sections in a couple of cases where the address of the
// section is insignificant to the user program and the behaviour matches that
// of the Visual C++ linker.
bool ICF::isEligible(SectionChunk *c) {
  // Non-comdat chunks, dead chunks, and writable chunks are not eligible.
  bool writable = c->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_MEM_WRITE;
  if (!c->isCOMDAT() || !c->live || writable)
    return false;

  // Under regular (not safe) ICF, all code sections are eligible.
  if ((ctx.config.doICF == ICFLevel::All) &&
      c->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
    return true;

  // .pdata and .xdata unwind info sections are eligible.
  StringRef outSecName = c->getSectionName().split('$').first;
  if (outSecName == ".pdata" || outSecName == ".xdata")
    return true;

  // So are vtables.
  const char *itaniumVtablePrefix =
      ctx.config.machine == I386 ? "__ZTV" : "_ZTV";
  if (c->sym && (c->sym->getName().starts_with("??_7") ||
                 c->sym->getName().starts_with(itaniumVtablePrefix)))
    return true;

  // Anything else not in an address-significance table is eligible.
  return !c->keepUnique;
}

// Split an equivalence class into smaller classes. The classes that came
// out with more than one section go to `out`; a section alone in its class
// goes to `alone`: it is never looked at again, so its class is copied into
// the other table once this iteration is over (not now: the other threads
// read that table).
void ICF::segregate(size_t begin, size_t end, bool constant,
                    std::vector<Range> &out,
                    std::vector<SectionChunk *> &alone) {
  while (begin < end) {
    // Divide [Begin, End) into two. Let Mid be the start index of the
    // second group.
    auto bound = std::stable_partition(
        chunks.begin() + begin + 1, chunks.begin() + end, [&](SectionChunk *s) {
          if (constant)
            return equalsConstant(chunks[begin], s);
          return equalsVariable(chunks[begin], s);
        });
    size_t mid = bound - chunks.begin();

    // Split [Begin, End) into [Begin, Mid) and [Mid, End). We use Mid as an
    // equivalence class ID because every group ends with a unique index.
    for (size_t i = begin; i < mid; ++i)
      chunks[i]->eqClass[(cnt + 1) % 2] = mid;
    if (mid - begin == 1)
      alone.push_back(chunks[begin]);
    else
      out.push_back({begin, mid});

    // If we created a group, we need to iterate the main loop again.
    if (mid != end)
      repeat = true;

    begin = mid;
  }
}

// Blocks of `ranges` of about the same number of sections, for working on
// the classes in parallel: the starts of the blocks, and the end.
std::vector<size_t> ICF::rangeBlocks() const {
  std::vector<size_t> starts{0};
  size_t total = 0;
  for (const Range &r : ranges)
    total += r.second - r.first;
  size_t perBlock = std::max<size_t>(total / 256, 1024);
  for (size_t i = 0, sum = 0; i < ranges.size(); ++i) {
    sum += ranges[i].second - ranges[i].first;
    if (sum >= perBlock) {
      starts.push_back(i + 1);
      sum = 0;
    }
  }
  if (starts.back() != ranges.size())
    starts.push_back(ranges.size());
  return starts;
}

// One iteration over the classes that could still split.
void ICF::segregateAll(bool constant) {
  std::vector<size_t> blocks = rangeBlocks();
  std::vector<std::vector<Range>> out(blocks.size() - 1);
  std::vector<std::vector<SectionChunk *>> alone(blocks.size() - 1);
  parallelFor(0, blocks.size() - 1, [&](size_t b) {
    for (size_t i = blocks[b]; i < blocks[b + 1]; ++i)
      segregate(ranges[i].first, ranges[i].second, constant, out[b], alone[b]);
  });
  // The sections now alone in their class keep it in both tables.
  parallelForEach(alone, [&](std::vector<SectionChunk *> &v) {
    for (SectionChunk *sc : v)
      sc->eqClass[cnt % 2] = sc->eqClass[(cnt + 1) % 2];
  });
  ranges.clear();
  for (std::vector<Range> &v : out)
    llvm::append_range(ranges, v);
  ++cnt;
}

bool ICF::assocEquals(const SectionChunk *a, const SectionChunk *b) {
  // Ignore associated metadata sections that don't participate in ICF, such as
  // debug info and CFGuard metadata.
  auto considerForICF = [](const SectionChunk &assoc) {
    StringRef Name = assoc.getSectionName();
    return !(Name.starts_with(".debug") || Name == ".gfids$y" ||
             Name == ".giats$y" || Name == ".gljmp$y");
  };
  auto ra = make_filter_range(a->children(), considerForICF);
  auto rb = make_filter_range(b->children(), considerForICF);
  return std::equal(ra.begin(), ra.end(), rb.begin(), rb.end(),
                    [&](const SectionChunk &ia, const SectionChunk &ib) {
                      return ia.eqClass[cnt % 2] == ib.eqClass[cnt % 2];
                    });
}

// Compare "non-moving" part of two sections, namely everything
// except relocation targets.
bool ICF::equalsConstant(const SectionChunk *a, const SectionChunk *b) {
  if (a->relocsSize != b->relocsSize)
    return false;

  // Compare relocations.
  auto eq = [&](const coff_relocation &r1, const coff_relocation &r2) {
    if (r1.Type != r2.Type ||
        r1.VirtualAddress != r2.VirtualAddress) {
      return false;
    }
    Symbol *b1 = a->file->getSymbol(r1.SymbolTableIndex);
    Symbol *b2 = b->file->getSymbol(r2.SymbolTableIndex);
    if (b1 == b2)
      return true;
    if (auto *d1 = dyn_cast<DefinedRegular>(b1))
      if (auto *d2 = dyn_cast<DefinedRegular>(b2))
        return d1->getValue() == d2->getValue() &&
               d1->getChunk()->eqClass[cnt % 2] == d2->getChunk()->eqClass[cnt % 2];
    return false;
  };
  if (!std::equal(a->getRelocs().begin(), a->getRelocs().end(),
                  b->getRelocs().begin(), eq))
    return false;

  // Compare section attributes and contents.
  return a->getOutputCharacteristics() == b->getOutputCharacteristics() &&
         a->getSectionName() == b->getSectionName() &&
         a->header->SizeOfRawData == b->header->SizeOfRawData &&
         a->checksum == b->checksum && a->getContents() == b->getContents() &&
         a->getMachine() == b->getMachine() && assocEquals(a, b);
}

// Compare "moving" part of two sections, namely relocation targets.
bool ICF::equalsVariable(const SectionChunk *a, const SectionChunk *b) {
  // Compare relocations.
  auto eqSym = [&](Symbol *b1, Symbol *b2) {
    if (b1 == b2)
      return true;
    if (auto *d1 = dyn_cast<DefinedRegular>(b1))
      if (auto *d2 = dyn_cast<DefinedRegular>(b2))
        return d1->getChunk()->eqClass[cnt % 2] == d2->getChunk()->eqClass[cnt % 2];
    return false;
  };
  auto eq = [&](const coff_relocation &r1, const coff_relocation &r2) {
    Symbol *b1 = a->file->getSymbol(r1.SymbolTableIndex);
    Symbol *b2 = b->file->getSymbol(r2.SymbolTableIndex);
    return eqSym(b1, b2);
  };

  Symbol *e1 = a->getEntryThunk();
  Symbol *e2 = b->getEntryThunk();
  if ((e1 || e2) && (!e1 || !e2 || !eqSym(e1, e2)))
    return false;

  return std::equal(a->getRelocs().begin(), a->getRelocs().end(),
                    b->getRelocs().begin(), eq) &&
         assocEquals(a, b);
}

// Merge identical COMDAT sections.
// Two sections are considered the same if their section headers,
// contents and relocations are all the same.
void ICF::run() {
  llvm::TimeTraceScope timeScope("ICF");
  ScopedTimer t(ctx.icfTimer);

  // Collect only mergeable sections and group by hash value. Whether a
  // section is eligible only depends on the section, so that is decided in
  // parallel.
  std::vector<Chunk *> all = ctx.driver.getChunks();
  std::vector<uint8_t> eligible(all.size());
  parallelFor(0, all.size(), [&](size_t i) {
    if (auto *sc = dyn_cast<SectionChunk>(all[i]))
      eligible[i] = isEligible(sc);
  });
  uint32_t nextId = 1;
  for (auto [i, c] : llvm::enumerate(all)) {
    if (auto *sc = dyn_cast<SectionChunk>(c)) {
      if (eligible[i])
        chunks.push_back(sc);
      else
        sc->eqClass[0] = sc->eqClass[1] = nextId++;
    }
  }

  // Make sure that ICF doesn't merge sections that are being handled by string
  // tail merging.
  for (MergeChunk *mc : ctx.mergeChunkInstances)
    if (mc)
      for (SectionChunk *sc : mc->sections)
        sc->eqClass[0] = sc->eqClass[1] = nextId++;

  // Initially, we use hash values to partition sections.
  parallelForEach(chunks, [&](SectionChunk *sc) {
    sc->eqClass[0] = xxh3_64bits(sc->getContents());
  });

  // Combine the hashes of the sections referenced by each section into its
  // hash.
  for (unsigned cnt = 0; cnt != 2; ++cnt) {
    parallelForEach(chunks, [&](SectionChunk *sc) {
      uint32_t hash = sc->eqClass[cnt % 2];
      for (Symbol *b : sc->symbols())
        if (auto *sym = dyn_cast_or_null<DefinedRegular>(b))
          hash += sym->getChunk()->eqClass[cnt % 2];
      // Set MSB to 1 to avoid collisions with non-hash classes.
      sc->eqClass[(cnt + 1) % 2] = hash | (1U << 31);
    });
  }

  // From now on, sections in Chunks are ordered so that sections in
  // the same group are consecutive in the vector. This is a stable sort by
  // eqClass[0], done as a parallel sort of (class, position) pairs: the
  // position breaks ties the way a stable sort would, and each class is read
  // once instead of through a pointer on every comparison. There are hundreds
  // of thousands of chunks on a large link. The groups with more than one
  // section are the classes to look at; a section alone in its group is
  // final and gets its class in both tables.
  {
    std::vector<std::pair<uint32_t, uint32_t>> keys(chunks.size());
    parallelFor(0, chunks.size(), [&](size_t i) {
      keys[i] = {chunks[i]->eqClass[0], static_cast<uint32_t>(i)};
    });
    parallelSort(keys, std::less<>());
    std::vector<SectionChunk *> sorted(chunks.size());
    parallelFor(0, keys.size(),
                [&](size_t i) { sorted[i] = chunks[keys[i].second]; });
    chunks = std::move(sorted);

    // Shard the groups: a shard starts at a group boundary.
    const size_t numShards = std::min<size_t>(256, keys.size() / 1024 + 1);
    std::vector<size_t> bounds(numShards + 1, keys.size());
    bounds[0] = 0;
    parallelFor(1, numShards, [&](size_t s) {
      size_t i = keys.size() * s / numShards;
      while (i < keys.size() && keys[i].first == keys[i - 1].first)
        ++i;
      bounds[s] = i;
    });
    std::vector<std::vector<Range>> found(numShards);
    parallelFor(0, numShards, [&](size_t s) {
      for (size_t i = bounds[s], end = bounds[s + 1]; i < end;) {
        size_t j = i + 1;
        while (j < end && keys[j].first == keys[i].first)
          ++j;
        if (j - i == 1)
          chunks[i]->eqClass[1] = chunks[i]->eqClass[0];
        else
          found[s].push_back({i, j});
        i = j;
      }
    });
    for (std::vector<Range> &v : found)
      llvm::append_range(ranges, v);
  }

  // Compare static contents and assign unique IDs for each static content.
  segregateAll(true);

  // Split groups by comparing relocations until convergence is obtained.
  do {
    repeat = false;
    segregateAll(false);
  } while (repeat);

  Log(ctx) << "ICF needed " << Twine(cnt) << " iterations";

  // Merge sections in the same classes.
  std::vector<size_t> blocks = rangeBlocks();
  parallelFor(0, blocks.size() - 1, [&](size_t b) {
    for (size_t r = blocks[b]; r < blocks[b + 1]; ++r) {
      auto [begin, end] = ranges[r];
      Log(ctx) << "Selected " << chunks[begin]->getDebugName();
      for (size_t i = begin + 1; i < end; ++i) {
        Log(ctx) << "  Removed " << chunks[i]->getDebugName();
        chunks[begin]->replace(chunks[i]);
      }
    }
  });
}

// Entry point to ICF.
void doICF(COFFLinkerContext &ctx) { ICF(ctx).run(); }

} // namespace lld::coff
