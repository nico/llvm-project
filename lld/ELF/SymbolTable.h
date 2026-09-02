//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYMBOL_TABLE_H
#define LLD_ELF_SYMBOL_TABLE_H

#include "Symbols.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Compiler.h"

namespace lld::elf {
struct Ctx;
class InputFile;
class SharedFile;

struct ArmCmseEntryFunction {
  Symbol *acleSeSym;
  Symbol *sym;
};

// SymbolTable is a bucket of all known symbols, including defined,
// undefined, or lazy symbols (the last one is symbols in archive
// files whose archive members are not yet loaded).
//
// We put all symbols of all files to a SymbolTable, and the
// SymbolTable selects the "best" symbols if there are name
// conflicts. For example, obviously, a defined symbol is better than
// an undefined symbol. Or, if there's a conflict between a lazy and a
// undefined, it'll read an archive member to read a real definition
// to replace the lazy symbol. The logic is implemented in the
// add*() functions, which are called by input files as they are parsed. There
// is one add* function per symbol type.
class SymbolTable {
public:
  SymbolTable(Ctx &ctx) : ctx(ctx) {}
  ArrayRef<Symbol *> getSymbols() const { return symVector; }
  SmallVector<Symbol *, 0> &getMutableSymbols() { return symVector; }

  void wrap(Symbol *sym, Symbol *real, Symbol *wrap);

  Symbol *insert(StringRef name);
  // Same, with the hash of the stem (the name without its @@version suffix)
  // computed in advance, off the serial path.
  Symbol *insert(llvm::CachedHashStringRef stem, StringRef name);

  // The map from symbol name to symbol is sharded by the top bits of the
  // name hash, so that symbol resolution (SymbolResolution.cpp) can add
  // symbols to all shards at once; each shard is owned by one thread then.
  static constexpr unsigned numShards = 32;
  static constexpr unsigned shardShift = 32 - 5;
  static constexpr unsigned slotBits = 26;
  static unsigned shardOf(uint32_t hash) { return hash >> shardShift; }
  struct Entry {
    Symbol *sym = nullptr;
    // Where symbol resolution keeps the symbol's per-batch data: the shard
    // that created the symbol, and its slot in that shard's syms.
    uint32_t home = 0;
  };
  struct Shard {
    llvm::DenseMap<llvm::CachedHashStringRef, Entry> map;
    SmallVector<Symbol *, 0> syms;
  };
  Shard &shard(unsigned i) { return shards[i]; }
  // The entry for a stem, created (with no symbol yet) if it did not exist.
  Entry &lookup(llvm::CachedHashStringRef stem, bool &isNew) {
    auto p = shards[shardOf(stem.hash())].map.try_emplace(stem);
    isNew = p.second;
    return p.first->second;
  }
  Entry *find(llvm::CachedHashStringRef stem) {
    auto &map = shards[shardOf(stem.hash())].map;
    auto it = map.find(stem);
    return it == map.end() ? nullptr : &it->second;
  }
  void erase(llvm::CachedHashStringRef stem) {
    shards[shardOf(stem.hash())].map.erase(stem);
  }
  uint32_t addSlot(unsigned s, Symbol *sym) {
    shards[s].syms.push_back(sym);
    return (s << slotBits) | (shards[s].syms.size() - 1);
  }
  void reserveSymVector(size_t n) { symVector.reserve(n); }
  void addToSymVector(Symbol *sym) { symVector.push_back(sym); }
  // The second half of insert(): names a new symbol, or renames an existing
  // one to its @@versioned name.
  static void initOrRename(Symbol *sym, bool isNew,
                           llvm::CachedHashStringRef stem, StringRef name);

  // Called by Symbol::resolve(); see SymbolResolution.cpp. During a batch,
  // these record what to do with the key of the current event; otherwise
  // they do it right away.
  void extract(const Symbol &sym, InputFile *file, const InputFile *reference,
               bool backref);
  void dismissBackref(const Symbol &sym);
  bool recordDiag(DiagLevel level, StringRef msg);

  template <typename T> Symbol *addSymbol(const T &newSym) {
    Symbol *sym = insert(newSym.getName());
    sym->resolve(ctx, newSym);
    return sym;
  }
  Symbol *addAndCheckDuplicate(Ctx &, const Defined &newSym);

  void scanVersionScript();

  Symbol *find(StringRef name);

  void handleDynamicList();

  Symbol *addUnusedUndefined(StringRef name,
                             uint8_t binding = llvm::ELF::STB_GLOBAL);

  // Set of .so files to not link the same shared object file more than once.
  llvm::DenseMap<llvm::CachedHashStringRef, SharedFile *> soNames;

  // Comdat groups define "link once" sections. If two comdat groups have the
  // same name, only one of them is linked, and the other is ignored. This map
  // is used to uniquify them. It is sharded like the symbol map so that the
  // groups of all files can be chosen in parallel (SymbolResolution.cpp).
  struct ComdatGroups {
    using Map = llvm::DenseMap<llvm::CachedHashStringRef, const InputFile *>;
    std::unique_ptr<Map[]> shards = std::make_unique<Map[]>(numShards);
    Map &shard(unsigned i) { return shards[i]; }
    // Registers the group for file if no earlier file has it; returns
    // whether it did.
    bool tryEmplace(llvm::CachedHashStringRef sig, const InputFile *file) {
      return shards[shardOf(sig.hash())].try_emplace(sig, file).second;
    }
    const InputFile *lookup(llvm::CachedHashStringRef sig) {
      return shards[shardOf(sig.hash())].lookup(sig);
    }
  };
  ComdatGroups comdatGroups;

  // The Map of __acle_se_<sym>, <sym> pairs found in the input objects.
  // Key is the <sym> name.
  llvm::SmallMapVector<StringRef, ArmCmseEntryFunction, 1> cmseSymMap;

  // Map of symbols defined in the Arm CMSE import library. The linker must
  // preserve the addresses in the output objects.
  llvm::StringMap<Defined *> cmseImportLib;

  // True if <sym> from the input Arm CMSE import library is written to the
  // output Arm CMSE import library.
  llvm::StringMap<bool> inCMSEOutImpLib;

private:
  SmallVector<Symbol *, 0> findByVersion(SymbolVersion ver);
  SmallVector<Symbol *, 0> findAllByVersion(SymbolVersion ver,
                                            bool includeNonDefault);

  llvm::StringMap<SmallVector<Symbol *, 0>> &getDemangledSyms();
  bool assignExactVersion(SymbolVersion ver, uint16_t versionId);
  void assignWildcardVersion(SymbolVersion ver, uint16_t versionId);

  Ctx &ctx;

  std::unique_ptr<Shard[]> shards = std::make_unique<Shard[]>(numShards);
  // Global symbols in insertion order (the order of .symtab).
  SmallVector<Symbol *, 0> symVector;

  // A map from demangled symbol names to their symbol objects.
  // This mapping is 1:N because two symbols with different versions
  // can have the same name. We use this map to handle "extern C++ {}"
  // directive in version scripts.
  std::optional<llvm::StringMap<SmallVector<Symbol *, 0>>> demangledSyms;
};

// A diagnostic from symbol resolution: printed right away, or, during a
// batch (SymbolResolution.cpp), recorded with the key of the current event
// and printed in that order at the end of the batch.
class ResolveDiag {
public:
  // Err is downgraded to a warning under --noinhibit-exec, like Err().
  ResolveDiag(Ctx &ctx, DiagLevel level)
      : ctx(ctx),
        level(level == DiagLevel::Err && ctx.arg.noinhibitExec ? DiagLevel::Warn
                                                               : level),
        os(ctx, DiagLevel::None) {}
  ~ResolveDiag();
  template <class T> const ResolveDiag &operator<<(T &&v) const {
    os << std::forward<T>(v);
    return *this;
  }
  uint64_t tell() { return os.tell(); }

private:
  Ctx &ctx;
  DiagLevel level;
  ELFSyncStream os;
};

} // namespace lld::elf

#endif
