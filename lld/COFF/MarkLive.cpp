//===- MarkLive.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "Symbols.h"
#include "lld/Common/Timer.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;

namespace lld::coff {

namespace {
// What marking a section live leads to marking as well: the sections its
// relocations refer to and its associative children (which are then walked
// too), the import files of the import symbols it refers to, and the thunks
// of the import thunk symbols. Collected by the parallel walk below, applied
// by one thread per shard of pointers (see MarkLiveImpl).
struct Candidates {
  std::vector<SectionChunk *> sections;
  std::vector<ImportFile *> importFiles;
  std::vector<ImportThunkChunk *> thunks;
};

// A pointer's shard, for spreading the marking over threads such that every
// flag is only ever written by one thread.
constexpr unsigned numShards = 64;
template <typename T> unsigned shardOf(const T *p) {
  return (reinterpret_cast<uintptr_t>(p) >> 4) % numShards;
}

class MarkLiveImpl {
public:
  MarkLiveImpl(COFFLinkerContext &ctx) : ctx(ctx) {}
  void run();

private:
  // Serial: marks what `s` refers to and puts newly live sections onto
  // `worklist`.
  void addSym(Symbol *s, SmallVectorImpl<SectionChunk *> &worklist);
  void addImportFile(ImportFile *file,
                     SmallVectorImpl<SectionChunk *> &worklist);
  void enqueue(SectionChunk *c, SmallVectorImpl<SectionChunk *> &worklist);
  void walk(SectionChunk *sc, SmallVectorImpl<SectionChunk *> &worklist);

  // Parallel: what `sc` leads to that is not live yet, by shard.
  void collect(SectionChunk *sc, Candidates *byShard);

  COFFLinkerContext &ctx;
};
} // namespace

void MarkLiveImpl::enqueue(SectionChunk *c,
                           SmallVectorImpl<SectionChunk *> &worklist) {
  if (c->live)
    return;
  c->live = true;
  worklist.push_back(c);
}

void MarkLiveImpl::addImportFile(ImportFile *file,
                                 SmallVectorImpl<SectionChunk *> &worklist) {
  file->live = true;
  if (file->impchkThunk && file->impchkThunk->exitThunk)
    addSym(file->impchkThunk->exitThunk, worklist);
}

void MarkLiveImpl::addSym(Symbol *s, SmallVectorImpl<SectionChunk *> &worklist) {
  Defined *b = s->getDefined();
  if (!b)
    return;
  if (auto *sym = dyn_cast<DefinedRegular>(b)) {
    enqueue(sym->getChunk(), worklist);
  } else if (auto *sym = dyn_cast<DefinedImportData>(b)) {
    addImportFile(sym->file, worklist);
  } else if (auto *sym = dyn_cast<DefinedImportThunk>(b)) {
    addImportFile(sym->wrappedSym->file, worklist);
    sym->getChunk()->live = true;
  }
}

void MarkLiveImpl::walk(SectionChunk *sc,
                        SmallVectorImpl<SectionChunk *> &worklist) {
  // Mark all symbols listed in the relocation table for this section.
  for (Symbol *b : sc->symbols())
    if (b)
      addSym(b, worklist);

  // Mark associative sections if any.
  for (SectionChunk &c : sc->children())
    enqueue(&c, worklist);

  // Mark EC entry thunks.
  if (Defined *entryThunk = sc->getEntryThunk())
    addSym(entryThunk, worklist);
}

// The parallel counterpart of walk(): only reads the live flags, and records
// what walk() would mark.
void MarkLiveImpl::collect(SectionChunk *sc, Candidates *byShard) {
  auto addImport = [&](ImportFile *file) {
    if (!file->live)
      byShard[shardOf(file)].importFiles.push_back(file);
  };
  for (Symbol *s : sc->symbols()) {
    if (!s)
      continue;
    Defined *b = s->getDefined();
    if (!b)
      continue;
    if (auto *sym = dyn_cast<DefinedRegular>(b)) {
      SectionChunk *c = sym->getChunk();
      if (!c->live)
        byShard[shardOf(c)].sections.push_back(c);
    } else if (auto *sym = dyn_cast<DefinedImportData>(b)) {
      addImport(sym->file);
    } else if (auto *sym = dyn_cast<DefinedImportThunk>(b)) {
      addImport(sym->wrappedSym->file);
      ImportThunkChunk *thunk = sym->getChunk();
      if (!thunk->live)
        byShard[shardOf(thunk)].thunks.push_back(thunk);
    }
  }
  for (SectionChunk &c : sc->children())
    if (!c.live)
      byShard[shardOf(&c)].sections.push_back(&c);
}

// Set live bit on for each reachable chunk. Unmarked (unreachable)
// COMDAT chunks will be ignored by Writer, so they will be excluded
// from the final output.
//
// The reachable set does not depend on the order things are found in, so
// the walk goes one level of the reference graph at a time, and a big level
// is done in parallel: walking the level's sections and collecting what they
// refer to that is not live yet only reads the live flags; then those
// candidates are marked, spread over threads by a hash of the pointer, so
// that every flag is only ever written by one thread. Small levels, and
// everything on ARM64EC (whose import files and sections lead to further
// symbols through thunks), take the one-thread path.
void MarkLiveImpl::run() {
  // We build up a worklist of sections which have been marked as live. We only
  // push into the worklist when we discover an unmarked section, and we mark
  // as we push, so sections never appear twice in the list.
  SmallVector<SectionChunk *, 256> worklist;

  // COMDAT section chunks are dead by default. Add non-COMDAT chunks. Do not
  // traverse DWARF sections. They are live, but they should not keep other
  // sections alive.
  for (Chunk *c : ctx.driver.getChunks())
    if (auto *sc = dyn_cast<SectionChunk>(c))
      if (sc->live && !sc->isDWARF())
        worklist.push_back(sc);

  // Add GC root chunks.
  for (Symbol *b : ctx.config.gcroot)
    addSym(b, worklist);

  const bool serial = isAnyArm64(ctx.config.machine) &&
                      ctx.config.machine != ARM64;
  constexpr size_t minParallelLevel = 2048;

  while (!worklist.empty()) {
    if (serial || worklist.size() < minParallelLevel) {
      SectionChunk *sc = worklist.pop_back_val();
      assert(sc->live && "We mark as live when pushing onto the worklist!");
      walk(sc, worklist);
      continue;
    }

    // The whole level at once: collect, per block of sections and shard.
    size_t numBlocks = std::min<size_t>(worklist.size() / 256, 4096);
    std::vector<std::vector<Candidates>> collected(numBlocks);
    parallelFor(0, numBlocks, [&](size_t block) {
      std::vector<Candidates> &byShard = collected[block];
      byShard.resize(numShards);
      size_t begin = worklist.size() * block / numBlocks;
      size_t end = worklist.size() * (block + 1) / numBlocks;
      for (size_t i = begin; i != end; ++i)
        collect(worklist[i], byShard.data());
    });
    worklist.clear();

    // Mark, one thread per shard; what is newly live is the next level.
    std::vector<std::vector<SectionChunk *>> next(numShards);
    parallelFor(0, numShards, [&](size_t shard) {
      for (std::vector<Candidates> &byShard : collected) {
        Candidates &c = byShard[shard];
        for (SectionChunk *sc : c.sections) {
          if (sc->live)
            continue;
          sc->live = true;
          next[shard].push_back(sc);
        }
        for (ImportFile *file : c.importFiles)
          file->live = true;
        for (ImportThunkChunk *thunk : c.thunks)
          thunk->live = true;
      }
    });
    for (std::vector<SectionChunk *> &v : next)
      worklist.append(v.begin(), v.end());
  }
}

void markLive(COFFLinkerContext &ctx) {
  llvm::TimeTraceScope timeScope("Mark live");
  ScopedTimer t(ctx.gcTimer);
  MarkLiveImpl(ctx).run();
}
} // namespace lld::coff
