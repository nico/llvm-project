//===- ICF.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ICF is short for Identical Code Folding. This is a size optimization to
// identify and merge two or more read-only sections (typically functions)
// that happened to have the same contents. It usually reduces output size
// by a few percent.
//
// In ICF, two sections are considered identical if they have the same
// section flags, section data, and relocations. Relocations are tricky,
// because two relocations are considered the same if they have the same
// relocation types, values, and if they point to the same sections *in
// terms of ICF*.
//
// Here is an example. If foo and bar defined below are compiled to the
// same machine instructions, ICF can and should merge the two, although
// their relocations point to each other.
//
//   void foo() { bar(); }
//   void bar() { foo(); }
//
// If you merge the two, their relocations point to the same section and
// thus you know they are mergeable, but how do you know they are
// mergeable in the first place? This is not an easy problem to solve.
//
// What we are doing in LLD is to partition sections into equivalence
// classes. Sections in the same equivalence class when the algorithm
// terminates are considered identical. Here are details:
//
// 1. First, we partition sections using their hash values as keys. Hash
//    values contain section types, section contents and numbers of
//    relocations. During this step, relocation targets are not taken into
//    account. We just put sections that apparently differ into different
//    equivalence classes.
//
// 2. Next, for each equivalence class, we visit sections to compare
//    relocation targets. Relocation targets are considered equivalent if
//    their targets are in the same equivalence class. Sections with
//    different relocation targets are put into different equivalence
//    classes.
//
// 3. If we split an equivalence class in step 2, two relocations
//    previously target the same equivalence class may now target
//    different equivalence classes. Therefore, we repeat step 2 until a
//    convergence is obtained.
//
// 4. For each equivalence class C, pick an arbitrary section in C, and
//    merge all the other sections in C with it.
//
// For small programs, this algorithm needs 3-5 iterations. For large
// programs such as Chromium, it takes more than 20 iterations.
//
// This algorithm was mentioned as an "optimistic algorithm" in [1],
// though gold implements a different algorithm than this.
//
// We parallelize each step so that multiple threads can work on different
// equivalence classes concurrently. That gave us a large performance
// boost when applying ICF on large programs. For example, MSVC link.exe
// or GNU gold takes 10-20 seconds to apply ICF on Chromium, whose output
// size is about 1.5 GB, but LLD can finish it in less than 2 seconds on a
// 2.8 GHz 40 core machine. Even without threading, LLD's ICF is still
// faster than MSVC or gold though.
//
// [1] Safe ICF: Pointer Safe and Unwinding aware Identical Code Folding
// in the Gold Linker
// http://static.googleusercontent.com/media/research.google.com/en//pubs/archive/36912.pdf
//
//===----------------------------------------------------------------------===//

#include "ICF.h"
#include "Config.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#include <atomic>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace lld;
using namespace lld::elf;

namespace {
template <class ELFT> class ICF {
public:
  ICF(Ctx &ctx) : ctx(ctx) {}
  void run();

private:
  void segregate(size_t begin, size_t end, uint32_t eqClassBase, bool constant);

  template <class RelTy>
  uint64_t constantRelocHash(const InputSection *sec, Relocs<RelTy> rels);

  template <class RelTy>
  bool constantEq(const InputSection *a, Relocs<RelTy> relsA,
                  const InputSection *b, Relocs<RelTy> relsB);

  bool equalsConstant(const InputSection *a, const RelsOrRelas<ELFT> &ra,
                      const InputSection *b);
  bool equalsVariable(const InputSection *a, const InputSection *b);

  using Range = std::pair<uint32_t, uint32_t>;

  void segregate(size_t begin, size_t end, uint32_t eqClassBase, bool constant,
                 std::vector<Range> &out, std::vector<InputSection *> &alone);
  void segregateAll(uint32_t eqClassBase, bool constant);
  std::vector<size_t> rangeBlocks() const;

  Ctx &ctx;
  SmallVector<InputSection *, 0> sections;
  std::vector<Range> ranges;
  std::vector<InputSection *> allTargets;

  // We repeat the main loop while `Repeat` is true.
  std::atomic<bool> repeat;

  // The main loop counter.
  int cnt = 0;

  // We have two locations for equivalence classes. On the first iteration
  // of the main loop, Class[0] has a valid value, and Class[1] contains
  // garbage. We read equivalence classes from slot 0 and write to slot 1.
  // So, Class[0] represents the current class, and Class[1] represents
  // the next class. On each iteration, we switch their roles and use them
  // alternately.
  //
  // Why are we doing this? Recall that other threads may be working on
  // other equivalence classes in parallel. They may read sections that we
  // are updating. We cannot update equivalence classes in place because
  // it breaks the invariance that all possibly-identical sections must be
  // in the same equivalence class at any moment. In other words, the for
  // loop to update equivalence classes is not atomic, and that is
  // observable from other threads. By writing new classes to other
  // places, we can keep the invariance.
  //
  // Below, `Current` has the index of the current class, and `Next` has
  // the index of the next class. If threading is enabled, they are either
  // (0, 1) or (1, 0).
  //
  // Note on single-thread: if that's the case, they are always (0, 0)
  // because we can safely read the next class without worrying about race
  // conditions. Using the same location makes this algorithm converge
  // faster because it uses results of the same iteration earlier.
  int current = 0;
  int next = 0;
};
}

// Returns true if section S is subject of ICF.
static bool isEligible(InputSection *s) {
  if (!s->isLive() || s->keepUnique || !(s->flags & SHF_ALLOC))
    return false;

  // Don't merge writable sections. .data.rel.ro sections are marked as writable
  // but are semantically read-only.
  if ((s->flags & SHF_WRITE) && s->name != ".data.rel.ro" &&
      !s->name.starts_with(".data.rel.ro."))
    return false;

  // SHF_LINK_ORDER sections are ICF'd as a unit with their dependent sections,
  // so we don't consider them for ICF individually.
  if (s->flags & SHF_LINK_ORDER)
    return false;

  // Don't merge synthetic sections as their Data member is not valid and empty.
  // The Data member needs to be valid for ICF as it is used by ICF to determine
  // the equality of section contents.
  if (isa<SyntheticSection>(s))
    return false;

  // .init and .fini contains instructions that must be executed to initialize
  // and finalize the process. They cannot and should not be merged.
  if (s->name == ".init" || s->name == ".fini")
    return false;

  // A user program may enumerate sections named with a C identifier using
  // __start_* and __stop_* symbols. We cannot ICF any such sections because
  // that could change program semantics.
  if (isValidCIdentifier(s->name))
    return false;

  return true;
}

// Split an equivalence class into smaller classes. The classes that came
// out with more than one section go to `out`; a section alone in its class
// goes to `alone`: it is never looked at again, so its class is copied into
// the other table once this iteration is over (not now: the other threads
// read that table).
template <class ELFT>
void ICF<ELFT>::segregate(size_t begin, size_t end, uint32_t eqClassBase,
                          bool constant, std::vector<Range> &out,
                          std::vector<InputSection *> &alone) {
  while (begin < end) {
    if (!constant) {
      if (sections[begin]->icfTargetCount == 0) {
        for (size_t i = begin; i < end; ++i)
          sections[i]->eqClass[next] = eqClassBase + end;
        out.push_back({begin, end});
        break;
      }
      auto bound =
          std::stable_partition(sections.begin() + begin + 1,
                                sections.begin() + end, [&](InputSection *s) {
                                  return equalsVariable(sections[begin], s);
                                });
      size_t mid = bound - sections.begin();
      for (size_t i = begin; i < mid; ++i)
        sections[i]->eqClass[next] = eqClassBase + mid;
      if (mid - begin == 1)
        alone.push_back(sections[begin]);
      else
        out.push_back({begin, mid});
      if (mid != end)
        repeat = true;
      begin = mid;
      continue;
    }

    // Divide [Begin, End) into two. Let Mid be the start index of the
    // second group.
    const RelsOrRelas<ELFT> ra =
        sections[begin]->template relsOrRelas<ELFT>(/*supportsCrel=*/false);
    auto bound =
        std::stable_partition(sections.begin() + begin + 1,
                              sections.begin() + end, [&](InputSection *s) {
                                return equalsConstant(sections[begin], ra, s);
                              });
    size_t mid = bound - sections.begin();

    // Now we split [Begin, End) into [Begin, Mid) and [Mid, End) by
    // updating the sections in [Begin, Mid). We use Mid as the basis for
    // the equivalence class ID because every group ends with a unique index.
    // Add this to eqClassBase to avoid equality with unique IDs.
    for (size_t i = begin; i < mid; ++i)
      sections[i]->eqClass[next] = eqClassBase + mid;
    if (mid - begin == 1)
      alone.push_back(sections[begin]);
    else
      out.push_back({begin, mid});

    // If we created a group, we need to iterate the main loop again.
    if (mid != end)
      repeat = true;

    begin = mid;
  }
}

// Hash the parts of a section's relocations that constantEq() compares: the
// offset, the type, and what the target resolves to at link time (an absolute
// value, an offset in an input section, or an offset in a merged output
// section). Two sections that constantEq() considers equal get equal hashes,
// so this only separates sections that segregate() would separate anyway,
// but it does so up front: without it, a class of thousands of sections with
// identical content and pairwise different relocation constants (e.g. small
// functions that each return a different string literal) costs segregate()
// a quadratic number of comparisons.
template <class ELFT>
template <class RelTy>
uint64_t ICF<ELFT>::constantRelocHash(const InputSection *sec,
                                      Relocs<RelTy> rels) {
  uint64_t hash = rels.size();
  const Symbol *const *symbols = sec->file->getSymbols().data();
  const bool isMips64EL = ctx.arg.isMips64EL;
  for (const RelTy &rel : rels) {
    uint64_t key =
        (uint64_t(rel.r_offset) << 8) ^ rel.getType(isMips64EL);
    const Symbol *s = symbols[rel.getSymbol(isMips64EL)];
    uint64_t addend = getAddend<ELFT>(rel);
    // Relocations that constantEq() only accepts as equal when they refer to
    // the same symbol with the same addend hash without their target.
    if (auto *d = dyn_cast<Defined>(s);
        d && !d->scriptDefined && !d->isPreemptible) {
      if (!d->section || isa<InputSection>(d->section)) {
        key ^= (d->value + addend) * 0x9E3779B97F4A7C15;
      } else if (auto *ms = dyn_cast<MergeInputSection>(d->section)) {
        uint64_t off = s->isSection() ? ms->getOffset(addend)
                                      : ms->getOffset(d->value) + addend;
        key ^= off * 0x9E3779B97F4A7C15;
      }
    }
    hash = (hash ^ key) * 0x9E3779B97F4A7C15;
  }
  return hash;
}

// Compare two lists of relocations.
template <class ELFT>
template <class RelTy>
bool ICF<ELFT>::constantEq(const InputSection *secA, Relocs<RelTy> ra,
                           const InputSection *secB, Relocs<RelTy> rb) {
  if (ra.size() != rb.size())
    return false;
  const Symbol *const *symbolsA = secA->file->getSymbols().data();
  const Symbol *const *symbolsB = secB->file->getSymbols().data();
  const bool isMips64EL = ctx.arg.isMips64EL;
  auto rai = ra.begin(), rae = ra.end(), rbi = rb.begin();
  for (; rai != rae; ++rai, ++rbi) {
    if (rai->r_offset != rbi->r_offset ||
        rai->getType(isMips64EL) != rbi->getType(isMips64EL))
      return false;

    uint64_t addA = getAddend<ELFT>(*rai);
    uint64_t addB = getAddend<ELFT>(*rbi);

    const Symbol *sa = symbolsA[rai->getSymbol(isMips64EL)];
    const Symbol *sb = symbolsB[rbi->getSymbol(isMips64EL)];
    if (sa == sb) {
      if (addA == addB)
        continue;
      return false;
    }

    auto *da = dyn_cast<Defined>(sa);
    auto *db = dyn_cast<Defined>(sb);

    // Placeholder symbols generated by linker scripts look the same now but
    // may have different values later.
    if (!da || !db || da->scriptDefined || db->scriptDefined)
      return false;

    // When comparing a pair of relocations, if they refer to different symbols,
    // and either symbol is preemptible, the containing sections should be
    // considered different. This is because even if the sections are identical
    // in this DSO, they may not be after preemption.
    if (da->isPreemptible || db->isPreemptible)
      return false;

    // Relocations referring to absolute symbols are constant-equal if their
    // values are equal.
    if (!da->section && !db->section && da->value + addA == db->value + addB)
      continue;
    if (!da->section || !db->section)
      return false;

    if (da->section->kind() != db->section->kind())
      return false;

    // Relocations referring to InputSections are constant-equal if their
    // section offsets are equal.
    if (isa<InputSection>(da->section)) {
      if (da->value + addA == db->value + addB)
        continue;
      return false;
    }

    // Relocations referring to MergeInputSections are constant-equal if their
    // offsets in the output section are equal.
    auto *x = dyn_cast<MergeInputSection>(da->section);
    if (!x)
      return false;
    auto *y = cast<MergeInputSection>(db->section);
    if (x->getParent() != y->getParent())
      return false;

    uint64_t offsetA =
        sa->isSection() ? x->getOffset(addA) : x->getOffset(da->value) + addA;
    uint64_t offsetB =
        sb->isSection() ? y->getOffset(addB) : y->getOffset(db->value) + addB;
    if (offsetA != offsetB)
      return false;
  }

  return true;
}

// Compare "non-moving" part of two InputSections, namely everything
// except relocation targets.
template <class ELFT>
bool ICF<ELFT>::equalsConstant(const InputSection *a,
                               const RelsOrRelas<ELFT> &ra,
                               const InputSection *b) {
  if (a->flags != b->flags || a->getSize() != b->getSize() ||
      a->getParent() != b->getParent() || a->content() != b->content())
    return false;

  if (ra.empty()) {
    if (b->relSecIdx == 0)
      return true;
    return b->template relsOrRelas<ELFT>(/*supportsCrel=*/false).empty();
  }
  if (b->relSecIdx == 0)
    return false;

  // A section is compared many times; decode CREL once (the decoded RELA
  // records are cached) rather than on every comparison.
  const RelsOrRelas<ELFT> rb =
      b->template relsOrRelas<ELFT>(/*supportsCrel=*/false);
  return ra.areRelocsRel() || rb.areRelocsRel()
             ? constantEq(a, ra.rels, b, rb.rels)
             : constantEq(a, ra.relas, b, rb.relas);
}

// Compare two lists of relocations. Returns true if all pairs of
// relocations point to the same section in terms of ICF.
// Compare "moving" part of two InputSections, namely relocation targets.
template <class ELFT>
bool ICF<ELFT>::equalsVariable(const InputSection *a, const InputSection *b) {
  assert(a->icfTargetCount == b->icfTargetCount);
  const InputSection *const *ta = allTargets.data() + a->icfTargetOff;
  const InputSection *const *tb = allTargets.data() + b->icfTargetOff;
  for (uint32_t i = 0, n = a->icfTargetCount; i < n; ++i) {
    const InputSection *x = ta[i];
    const InputSection *y = tb[i];
    if (x == y)
      continue;
    uint32_t cx = x->eqClass[current];
    uint32_t cy = y->eqClass[current];
    if (cx == 0 || cx != cy)
      return false;
  }
  return true;
}

// Blocks of `ranges` of about the same number of sections, for working on
// the classes in parallel: the starts of the blocks, and the end.
template <class ELFT>
std::vector<size_t> ICF<ELFT>::rangeBlocks() const {
  std::vector<size_t> starts{0};
  size_t total = 0;
  for (const Range &r : ranges)
    total += r.second - r.first;
  size_t perBlock = std::max<size_t>(total / 256, 64);
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
template <class ELFT>
void ICF<ELFT>::segregateAll(uint32_t eqClassBase, bool constant) {
  current = cnt % 2;
  next = (cnt + 1) % 2;
  std::vector<size_t> blocks = rangeBlocks();
  std::vector<std::vector<Range>> out(blocks.size() - 1);
  std::vector<std::vector<InputSection *>> alone(blocks.size() - 1);
  parallelFor(0, blocks.size() - 1, [&](size_t b) {
    for (size_t i = blocks[b]; i < blocks[b + 1]; ++i)
      segregate(ranges[i].first, ranges[i].second, eqClassBase, constant,
                out[b], alone[b]);
  });
  // The sections now alone in their class keep it in both tables.
  parallelForEach(alone, [&](std::vector<InputSection *> &v) {
    for (InputSection *s : v)
      s->eqClass[current] = s->eqClass[next];
  });
  ranges.clear();
  for (std::vector<Range> &v : out)
    llvm::append_range(ranges, v);
  ++cnt;
}

// Combine the hashes of the sections referenced by the given section into its
// hash.
template <class RelTy>
static void combineRelocHashes(Ctx &ctx, unsigned cnt, InputSection *isec,
                               Relocs<RelTy> rels) {
  uint32_t hash = isec->eqClass[cnt % 2];
  const Symbol *const *symbols = isec->file->getSymbols().data();
  const bool isMips64EL = ctx.arg.isMips64EL;
  for (RelTy rel : rels) {
    const Symbol *s = symbols[rel.getSymbol(isMips64EL)];
    if (auto *d = dyn_cast<Defined>(s))
      if (auto *relSec = dyn_cast_or_null<InputSection>(d->section))
        hash += relSec->eqClass[cnt % 2];
  }
  // Set MSB to 1 to avoid collisions with unique IDs.
  isec->eqClass[(cnt + 1) % 2] = hash | (1U << 31);
}

struct KeySec {
  uint32_t eqClass;
  InputSection *sec;
};

static void parallelRadixSortSections(MutableArrayRef<InputSection *> sections) {
  const size_t n = sections.size();
  std::vector<KeySec> keys(n);
  std::vector<KeySec> scratch(n);

  parallelFor(0, n, [&](size_t i) {
    keys[i] = {sections[i]->eqClass[0], sections[i]};
  });

  KeySec *src = keys.data();
  KeySec *dst = scratch.data();

  size_t numThreads = parallel::strategy.ThreadsRequested;
  if (numThreads <= 1 || n < 16384) {
    for (int shift = 0; shift < 24; shift += 8) {
      uint32_t counts[256] = {};
      for (size_t i = 0; i < n; ++i)
        counts[(src[i].eqClass >> shift) & 255]++;

      uint32_t offsets[256];
      offsets[0] = 0;
      for (size_t i = 1; i < 256; ++i)
        offsets[i] = offsets[i - 1] + counts[i - 1];

      for (size_t i = 0; i < n; ++i) {
        auto val = src[i];
        dst[offsets[(val.eqClass >> shift) & 255]++] = val;
      }
      std::swap(src, dst);
    }
    // Final pass directly into sections.
    uint32_t counts[256] = {};
    for (size_t i = 0; i < n; ++i)
      counts[(src[i].eqClass >> 24) & 255]++;
    uint32_t offsets[256];
    offsets[0] = 0;
    for (size_t i = 1; i < 256; ++i)
      offsets[i] = offsets[i - 1] + counts[i - 1];
    for (size_t i = 0; i < n; ++i)
      sections[offsets[(src[i].eqClass >> 24) & 255]++] = src[i].sec;
    return;
  }

  numThreads = std::min<size_t>(numThreads, 64);
  size_t chunkSize = (n + numThreads - 1) / numThreads;

  std::vector<std::array<uint32_t, 256>> threadCounts(numThreads);
  std::vector<std::array<uint32_t, 256>> threadOffsets(numThreads);

  for (int shift = 0; shift < 24; shift += 8) {
    parallelFor(0, numThreads, [&](size_t t) {
      size_t begin = t * chunkSize;
      size_t end = std::min(begin + chunkSize, n);
      auto &counts = threadCounts[t];
      counts.fill(0);
      for (size_t i = begin; i < end; ++i)
        counts[(src[i].eqClass >> shift) & 255]++;
    });

    uint32_t runningSum = 0;
    for (size_t b = 0; b < 256; ++b) {
      for (size_t t = 0; t < numThreads; ++t) {
        threadOffsets[t][b] = runningSum;
        runningSum += threadCounts[t][b];
      }
    }

    parallelFor(0, numThreads, [&](size_t t) {
      size_t begin = t * chunkSize;
      size_t end = std::min(begin + chunkSize, n);
      auto offsets = threadOffsets[t];
      for (size_t i = begin; i < end; ++i) {
        auto val = src[i];
        dst[offsets[(val.eqClass >> shift) & 255]++] = val;
      }
    });

    std::swap(src, dst);
  }

  // Final pass directly writes to sections.
  parallelFor(0, numThreads, [&](size_t t) {
    size_t begin = t * chunkSize;
    size_t end = std::min(begin + chunkSize, n);
    auto &counts = threadCounts[t];
    counts.fill(0);
    for (size_t i = begin; i < end; ++i)
      counts[(src[i].eqClass >> 24) & 255]++;
  });

  uint32_t runningSum = 0;
  for (size_t b = 0; b < 256; ++b) {
    for (size_t t = 0; t < numThreads; ++t) {
      threadOffsets[t][b] = runningSum;
      runningSum += threadCounts[t][b];
    }
  }

  parallelFor(0, numThreads, [&](size_t t) {
    size_t begin = t * chunkSize;
    size_t end = std::min(begin + chunkSize, n);
    auto offsets = threadOffsets[t];
    for (size_t i = begin; i < end; ++i) {
      auto val = src[i];
      sections[offsets[(val.eqClass >> 24) & 255]++] = val.sec;
    }
  });
}

// The main function of ICF.
template <class ELFT> void ICF<ELFT>::run() {
  // Two text sections may have identical content and relocations but different
  // LSDA, e.g. the two functions may have catch blocks of different types. If a
  // text section is referenced by a .eh_frame FDE with LSDA, it is not
  // eligible. This is implemented by iterating over CIE/FDE and setting
  // eqClass[0] to the referenced text section from a live FDE.
  //
  // If two .gcc_except_table have identical semantics (usually identical
  // content with PC-relative encoding), we will lose folding opportunity.
  uint32_t uniqueId = 0;
  ctx.in.ehFrame->iterateFDEWithLSDA<ELFT>(
      [&](InputSection &s) { s.eqClass[0] = s.eqClass[1] = ++uniqueId; });

  // Collect sections to merge.
  {
    llvm::TimeTraceScope timeScope("Collect sections");
    size_t numShards = parallel::strategy.ThreadsRequested;
    if (numShards == 0)
      numShards = 1;
    size_t numSections = ctx.inputSections.size();
    size_t chunkSize = (numSections + numShards - 1) / numShards;
    SmallVector<SmallVector<InputSection *, 0>, 0> shards(numShards);
    uint32_t baseId = uniqueId;

    parallelFor(0, numShards, [&](size_t i) {
      size_t begin = i * chunkSize;
      size_t end = std::min(begin + chunkSize, numSections);
      for (size_t j = begin; j < end; ++j) {
        auto *s = dyn_cast<InputSection>(ctx.inputSections[j]);
        if (s && s->eqClass[0] == 0) {
          if (isEligible(s))
            shards[i].push_back(s);
          else
            s->eqClass[0] = s->eqClass[1] = baseId + 1 + j;
        }
      }
    });

    uniqueId = baseId + numSections;

    size_t totalEligible = 0;
    for (const auto &shard : shards)
      totalEligible += shard.size();
    sections.reserve(totalEligible);
    for (auto &shard : shards)
      sections.insert(sections.end(), shard.begin(), shard.end());
  }

  {
    llvm::TimeTraceScope timeScope("Hash sections");
    // Initially, we use hash values to partition sections: the hash of the
    // content and of the constant parts of the relocations.
    parallelForEach(sections, [&](InputSection *s) {
      uint64_t hash = xxh3_64bits(s->content());
      if (s->relSecIdx != 0) {
        const RelsOrRelas<ELFT> rels = s->template relsOrRelas<ELFT>();
        if (rels.areRelocsCrel())
          hash ^= constantRelocHash(s, rels.crels);
        else if (rels.areRelocsRel())
          hash ^= constantRelocHash(s, rels.rels);
        else
          hash ^= constantRelocHash(s, rels.relas);
      }
      hash ^= hash >> 32;
      // Set MSB to 1 to avoid collisions with unique IDs.
      s->eqClass[0] = hash | (1U << 31);
      s->eqClass[1] = s->eqClass[0];
    });

    // Perform 2 rounds of relocation hash propagation. 2 is an empirical value
    // to reduce the average sizes of equivalence classes, i.e. segregate()
    // which has a large time complexity will have less work to do.
    for (unsigned cnt = 0; cnt != 2; ++cnt) {
      parallelForEach(sections, [&](InputSection *s) {
        if (s->relSecIdx == 0)
          return;
        const RelsOrRelas<ELFT> rels = s->template relsOrRelas<ELFT>();
        if (rels.areRelocsCrel())
          combineRelocHashes(ctx, cnt, s, rels.crels);
        else if (rels.areRelocsRel())
          combineRelocHashes(ctx, cnt, s, rels.rels);
        else
          combineRelocHashes(ctx, cnt, s, rels.relas);
      });
    }
  }

  {
    llvm::TimeTraceScope timeScope("Sort sections");
    // From now on, sections in Sections vector are ordered so that sections
    // in the same equivalence class are consecutive in the vector. This is a
    // stable sort by eqClass[0], done using a 4-pass parallel radix sort.
    parallelRadixSortSections(sections);

    // Shard the groups: a shard starts at a group boundary.
    const size_t numSections = sections.size();
    const size_t numShards = std::min<size_t>(256, numSections / 1024 + 1);
    std::vector<size_t> bounds(numShards + 1, numSections);
    bounds[0] = 0;
    parallelFor(1, numShards, [&](size_t s) {
      size_t i = numSections * s / numShards;
      while (i < numSections &&
             sections[i]->eqClass[0] == sections[i - 1]->eqClass[0])
        ++i;
      bounds[s] = i;
    });
    std::vector<std::vector<Range>> found(numShards);
    parallelFor(0, numShards, [&](size_t s) {
      for (size_t i = bounds[s], end = bounds[s + 1]; i < end;) {
        size_t j = i + 1;
        while (j < end && sections[j]->eqClass[0] == sections[i]->eqClass[0])
          ++j;
        if (j - i == 1)
          sections[i]->eqClass[1] = sections[i]->eqClass[0];
        else
          found[s].push_back({i, j});
        i = j;
      }
    });
    for (std::vector<Range> &v : found)
      llvm::append_range(ranges, v);
  }

  // Compare static contents and assign unique equivalence class IDs for each
  // static content. Use a base offset for these IDs to ensure no overlap with
  // the unique IDs already assigned.
  uint32_t eqClassBase = ++uniqueId;
  {
    llvm::TimeTraceScope timeScope("Segregate by constant parts");
    segregateAll(eqClassBase, true);
  }

  // Extract target InputSections for all surviving ranges before
  // variable segregation so that equalsVariable can compare them directly
  // without repeatedly querying symbols and relocation records.
  size_t numBlocks =
      std::min<size_t>(ranges.size(), parallel::strategy.ThreadsRequested);
  if (numBlocks == 0)
    numBlocks = 1;
  size_t rangesPerBlock = (ranges.size() + numBlocks - 1) / numBlocks;

  struct PerBlock {
    std::vector<InputSection *> targets;
    std::vector<std::pair<InputSection *, uint32_t>> secOffsets;
  };
  std::vector<PerBlock> perBlock(numBlocks);

  parallelFor(0, numBlocks, [&](size_t b) {
    size_t begin = b * rangesPerBlock;
    size_t end = std::min(begin + rangesPerBlock, ranges.size());
    PerBlock &pb = perBlock[b];
    const bool isMips64EL = ctx.arg.isMips64EL;

    for (size_t r = begin; r < end; ++r) {
      for (size_t i = ranges[r].first; i < ranges[r].second; ++i) {
        InputSection *s = sections[i];
        if (s->relSecIdx == 0) {
          s->icfTargetOff = 0;
          s->icfTargetCount = 0;
          continue;
        }
        uint32_t localStart = pb.targets.size();
        const RelsOrRelas<ELFT> rels =
            s->template relsOrRelas<ELFT>(/*supportsCrel=*/false);
        const Symbol *const *symbols = s->file->getSymbols().data();
        auto collect = [&](auto rel) {
          const Symbol *sym = symbols[rel.getSymbol(isMips64EL)];
          if (auto *d = dyn_cast<Defined>(sym))
            if (d->section)
              if (auto *isec = dyn_cast<InputSection>(d->section))
                pb.targets.push_back(isec);
        };
        if (rels.areRelocsRel())
          for (auto rel : rels.rels)
            collect(rel);
        else
          for (auto rel : rels.relas)
            collect(rel);

        uint32_t cnt = pb.targets.size() - localStart;
        s->icfTargetCount = cnt;
        if (cnt == 0) {
          s->icfTargetOff = 0;
        } else {
          pb.secOffsets.push_back({s, localStart});
        }
      }
    }
  });

  std::vector<size_t> blockOffsets(numBlocks + 1, 0);
  for (size_t b = 0; b < numBlocks; ++b)
    blockOffsets[b + 1] = blockOffsets[b] + perBlock[b].targets.size();

  allTargets.resize(blockOffsets.back());

  parallelFor(0, numBlocks, [&](size_t b) {
    size_t globalBase = blockOffsets[b];
    if (!perBlock[b].targets.empty())
      memcpy(allTargets.data() + globalBase, perBlock[b].targets.data(),
             perBlock[b].targets.size() * sizeof(InputSection *));
    for (auto &item : perBlock[b].secOffsets)
      item.first->icfTargetOff = globalBase + item.second;
  });

  // Split groups by comparing relocations until convergence is obtained.
  do {
    llvm::TimeTraceScope timeScope("Segregate by relocation targets");
    repeat = false;
    segregateAll(eqClassBase, false);
  } while (repeat);

  Log(ctx) << "ICF needed " << cnt << " iterations";
  llvm::TimeTraceScope timeScope("Merge sections");

  // Merge sections by the equivalence class. The classes are disjoint, so
  // this runs in parallel unless the messages of --print-icf-sections need to
  // come out in order.
  bool print = ctx.arg.printIcfSections;
  auto merge = [&](size_t begin, size_t end) {
    if (end - begin == 1)
      return;
    if (print)
      Msg(ctx) << "selected section " << sections[begin];
    for (size_t i = begin + 1; i < end; ++i) {
      if (print)
        Msg(ctx) << "  removing identical section " << sections[i];
      sections[begin]->replace(sections[i]);

      // At this point we know sections merged are fully identical and hence
      // we want to remove duplicate implicit dependencies such as link order
      // and relocation sections.
      for (InputSection *isec : sections[i]->dependentSections)
        isec->markDead();
    }
  };
  if (print) {
    for (const Range &r : ranges)
      merge(r.first, r.second);
  } else {
    std::vector<size_t> blocks = rangeBlocks();
    parallelFor(0, blocks.size() - 1, [&](size_t b) {
      for (size_t r = blocks[b]; r < blocks[b + 1]; ++r)
        merge(ranges[r].first, ranges[r].second);
    });
  }

  // Change Defined symbol's section field to the canonical one.
  auto fold = [](Symbol *sym) {
    if (auto *d = dyn_cast<Defined>(sym))
      if (d->section && d->section->kind() == SectionBase::Regular) {
        auto *sec = static_cast<InputSection *>(d->section);
        if (sec->repl != sec) {
          d->section = sec->repl;
          d->folded = true;
        }
      }
  };
  parallelForEach(ctx.symtab->getSymbols(), fold);
  parallelForEach(ctx.objectFiles, [&](ELFFileBase *file) {
    if (!file->hasFoldedSections.load(std::memory_order_relaxed))
      return;
    for (Symbol *sym : file->getLocalSymbols())
      fold(sym);
  });

  // InputSectionDescription::sections is populated by processSectionCommands().
  // ICF may fold some input sections assigned to output sections. Remove them.
  parallelForEach(ctx.script->sectionCommands, [](SectionCommand *cmd) {
    if (auto *osd = dyn_cast<OutputDesc>(cmd))
      for (SectionCommand *subCmd : osd->osec.commands)
        if (auto *isd = dyn_cast<InputSectionDescription>(subCmd))
          llvm::erase_if(isd->sections,
                         [](InputSection *isec) { return !isec->isLive(); });
  });

  // Reset outSecOff which was temporarily reused for icfTargetOff/icfTargetCount.
  parallelForEach(sections, [](InputSection *s) { s->outSecOff = 0; });
}

// ICF entry point function.
template <class ELFT> void elf::doIcf(Ctx &ctx) {
  llvm::TimeTraceScope timeScope("ICF");
  ICF<ELFT>(ctx).run();
}

template void elf::doIcf<ELF32LE>(Ctx &);
template void elf::doIcf<ELF32BE>(Ctx &);
template void elf::doIcf<ELF64LE>(Ctx &);
template void elf::doIcf<ELF64BE>(Ctx &);
