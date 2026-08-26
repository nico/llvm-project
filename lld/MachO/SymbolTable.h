//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_SYMBOL_TABLE_H
#define LLD_MACHO_SYMBOL_TABLE_H

#include "Symbols.h"

#include "lld/Common/LLVM.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Object/Archive.h"

namespace lld::macho {

class ArchiveFile;
class DylibFile;
class InputFile;
class ObjFile;
class InputSection;
class MachHeaderSection;
class Symbol;
class Defined;
class Undefined;

/*
 * Note that the SymbolTable handles name collisions by calling
 * replaceSymbol(), which does an in-place update of the Symbol via `placement
 * new`. Therefore, there is no need to update any relocations that hold
 * pointers the "old" Symbol -- they will automatically point to the new one.
 */
class SymbolTable {
public:
  // `known`, if given, is the symbol this name already maps to, saving a
  // lookup. ObjFile::parseLazy() has looked up every name it defines, so
  // parsing an archive member would otherwise hash them all a second time.
  Defined *addDefined(llvm::CachedHashStringRef name, InputFile *,
                      InputSection *, uint64_t value, uint64_t size,
                      bool isWeakDef, bool isPrivateExtern,
                      bool isReferencedDynamically, bool noDeadStrip,
                      bool isWeakDefCanBeHidden, bool isCold = false,
                      Symbol *known = nullptr);
  Defined *addDefined(StringRef name, InputFile *file, InputSection *isec,
                      uint64_t value, uint64_t size, bool isWeakDef,
                      bool isPrivateExtern, bool isReferencedDynamically,
                      bool noDeadStrip, bool isWeakDefCanBeHidden,
                      bool isCold = false, Symbol *known = nullptr) {
    return addDefined(llvm::CachedHashStringRef(name), file, isec, value, size,
                      isWeakDef, isPrivateExtern, isReferencedDynamically,
                      noDeadStrip, isWeakDefCanBeHidden, isCold, known);
  }

  Defined *aliasDefined(Defined *src, StringRef target, InputFile *newFile,
                        bool makePrivateExtern = false);

  Symbol *addUndefined(llvm::CachedHashStringRef name, InputFile *,
                       bool isWeakRef);
  Symbol *addUndefined(StringRef name, InputFile *file, bool isWeakRef) {
    return addUndefined(llvm::CachedHashStringRef(name), file, isWeakRef);
  }

  Symbol *addCommon(llvm::CachedHashStringRef name, InputFile *, uint64_t size,
                    uint32_t align, bool isPrivateExtern);
  Symbol *addCommon(StringRef name, InputFile *file, uint64_t size,
                    uint32_t align, bool isPrivateExtern) {
    return addCommon(llvm::CachedHashStringRef(name), file, size, align,
                     isPrivateExtern);
  }

  Symbol *addDylib(llvm::CachedHashStringRef name, DylibFile *file,
                   bool isWeakDef, bool isTlv);
  Symbol *addDylib(StringRef name, DylibFile *file, bool isWeakDef,
                   bool isTlv) {
    return addDylib(llvm::CachedHashStringRef(name), file, isWeakDef, isTlv);
  }
  Symbol *addDynamicLookup(StringRef name);

  Symbol *addLazyArchive(StringRef name, ArchiveFile *file,
                         const llvm::object::Archive::Symbol &sym);
  Symbol *addLazyObject(llvm::CachedHashStringRef name, InputFile &file);
  Symbol *addLazyObject(StringRef name, InputFile &file) {
    return addLazyObject(llvm::CachedHashStringRef(name), file);
  }

  Defined *addSynthetic(StringRef name, InputSection *, uint64_t value,
                        bool isPrivateExtern, bool includeInSymtab,
                        bool referencedDynamically);

  // All symbols, in the order they were added. See EventKey for how that
  // order is defined. Only complete outside a batch, see endBatch().
  ArrayRef<Symbol *> getSymbols() const { return symVector; }
  Symbol *find(llvm::CachedHashStringRef name);
  Symbol *find(StringRef name) { return find(llvm::CachedHashStringRef(name)); }

  // Symbols are added from batches of input files, see parseLater(), and by
  // the driver in between. Each addition is tagged with a key that says where
  // it happened: the batch, the file and the position within the file, or a
  // running counter for additions from the driver. Lists that have to come out
  // in the order things happened (the symbols themselves, duplicate symbol
  // diagnostics) are sorted by this key, so they no longer depend on the order
  // in which the additions were actually performed.
  using EventKey = uint64_t;
  static EventKey makeEventKey(uint32_t batch, uint32_t file, uint32_t event) {
    return (EventKey(batch) << 48) | (EventKey(file) << 24) | event;
  }
  // Mark the start and end of a batch. Symbols added by a batch are collected
  // per shard and only merged into getSymbols() by endBatch(), in key order;
  // the driver's additions before and after a batch get keys that sort before
  // and after the batch's, and go straight onto the list.
  void beginBatch() { ++batch; }
  void endBatch();
  // The key for additions from the current file event, per shard: the code
  // that replays a file's symbols into a shard sets it before each event, and
  // resets it to 0, "from the driver", when done.
  void setCurrentEvent(size_t shard, EventKey key);
  size_t shardIndex(llvm::CachedHashStringRef name) const {
    return name.hash() >> (32 - shardBits);
  }
  uint32_t currentBatch() const { return batch; }
  static constexpr unsigned shardBits = 6;
  static constexpr size_t numShards = 1 << shardBits;

  // Reports all duplicate symbols recorded by addDefined(), in key order.
  void reportDuplicateSymbols();

  // While the symbols of a batch of files are being added from several
  // threads at once (one per shard), the operations that would touch state
  // shared between shards -- extracting an archive member, fetching one
  // through the archive index, moving symbols between sections when weak
  // definitions get coalesced -- are only recorded, per shard. Afterwards,
  // finishParallelReplay() performs them, in key order, which is the order
  // they would have happened in one file at a time.
  void beginParallelReplay();
  void finishParallelReplay();
  bool inParallelReplay() const { return parallelReplay; }

  // Moving local symbols between sections when weak definitions get
  // coalesced is recorded during a parallel replay too, with the key of the
  // addDefined() that caused it. It is applied by the caller, interleaved
  // with registering the external symbols with their sections: see
  // parseBatch() for why the order matters.
  struct Transplant {
    EventKey key;
    InputSection *fromIsec;
    InputSection *toIsec;
    Defined *skip;
    uint64_t fromOff;
    uint64_t toOff;
  };
  // All transplants recorded since the last call, in key order.
  std::vector<Transplant> takeTransplants();
  static void applyTransplant(const Transplant &t);

  struct DuplicateSymbolDiag {
    EventKey key;
    // Pair containing source location and source file
    std::pair<std::string, std::string> src1;
    std::pair<std::string, std::string> src2;
    const Symbol *sym;
  };

private:
  std::pair<Symbol *, bool> insert(llvm::CachedHashStringRef name,
                                   const InputFile *);
  std::pair<Symbol *, bool> insert(StringRef name, const InputFile *file) {
    return insert(llvm::CachedHashStringRef(name), file);
  }

  // The table is split into shards by name hash. Each shard is only ever
  // touched by one thread at a time, which is what lets the symbols of many
  // files be added in parallel: every name lands in exactly one shard.
  struct Extraction {
    EventKey key;
    InputFile *file;
    StringRef reason;
  };
  struct Fetch {
    EventKey key;
    ArchiveFile *file;
    llvm::object::Archive::Symbol sym;
  };
  struct Shard;
  void extractFile(Shard &shard, InputFile &file, StringRef reason);
  void fetchMember(Shard &shard, ArchiveFile *file,
                   const llvm::object::Archive::Symbol &sym);
  void transplantSymbols(Shard &shard, InputSection *fromIsec,
                         InputSection *toIsec, Defined *skip, uint64_t fromOff,
                         uint64_t toOff);

  struct Shard {
    // Maps a name to its symbol. Note this holds the Symbol * directly rather
    // than an index into a list, so that the list order can be defined
    // independently of the map.
    llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> map;
    EventKey currentEvent = 0;
    // Recorded during a parallel replay, see finishParallelReplay().
    std::vector<Extraction> extractions;
    std::vector<Fetch> fetches;
    std::vector<Transplant> transplants;
    // Symbols added by the current batch, see endBatch().
    std::vector<std::pair<EventKey, Symbol *>> pending;
    std::vector<DuplicateSymbolDiag> dupSymDiags;
  };
  EventKey nextKey(Shard &shard);
  std::vector<Shard> shards{numShards};
  // Shard by the high bits of the hash: DenseMap picks buckets by the low
  // bits, so those have to stay evenly distributed within each shard.
  Shard &shardFor(llvm::CachedHashStringRef name) {
    return shards[shardIndex(name)];
  }

  uint32_t batch = 0;
  uint32_t driverCounter = 0;
  bool parallelReplay = false;
  // getSymbols()'s result, and the keys it is sorted by.
  std::vector<Symbol *> symVector;
  std::vector<EventKey> symVectorKeys;
};

void reportPendingUndefinedSymbols();
void reportPendingDuplicateSymbols();

// Call reportPendingUndefinedSymbols() to emit diagnostics.
void treatUndefinedSymbol(const Undefined &, StringRef source);
void treatUndefinedSymbol(const Undefined &, const InputSection *,
                          uint64_t offset);

extern std::unique_ptr<SymbolTable> symtab;

} // namespace lld::macho

#endif
