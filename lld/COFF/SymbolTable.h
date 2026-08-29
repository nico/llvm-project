//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_SYMBOL_TABLE_H
#define LLD_COFF_SYMBOL_TABLE_H

#include "InputFiles.h"
#include "LTO.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
struct LTOCodeGenerator;
}

namespace lld::coff {

class Chunk;
class CommonChunk;
class COFFLinkerContext;
class Defined;
class DefinedAbsolute;
class DefinedRegular;
class ImportThunkChunk;
class LazyArchive;
class SameAddressThunkARM64EC;
class SectionChunk;
class Symbol;

// This data structure is instantiated for each -wrap option.
struct WrappedSymbol {
  Symbol *sym;
  Symbol *real;
  Symbol *wrap;
};

struct UndefinedDiag;

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
// add*() functions, which are called by input files as they are parsed.
// There is one add* function per symbol type.
class SymbolTable {
public:
  SymbolTable(COFFLinkerContext &c,
              llvm::COFF::MachineTypes machine = IMAGE_FILE_MACHINE_UNKNOWN)
      : ctx(c), machine(machine) {}

  // Emit errors for symbols that cannot be resolved.
  void reportUnresolvable();

  // Try to resolve any undefined symbols and update the symbol table
  // accordingly, then print an error message for any remaining undefined
  // symbols and warn about imported local symbols.
  void resolveRemainingUndefines(std::vector<Undefined *> &aliases);

  // Try to resolve undefined symbols with alternate names.
  void resolveAlternateNames();

  // Load lazy objects that are needed for MinGW automatic import and for
  // doing stdcall fixups.
  void loadMinGWSymbols();
  bool handleMinGWAutomaticImport(Symbol *sym, StringRef name);

  // Returns a symbol for a given name. Returns a nullptr if not found.
  Symbol *find(StringRef name) const;
  Symbol *findUnderscore(StringRef name) const;

  void addUndefinedGlob(StringRef arg);

  // Occasionally we have to resolve an undefined symbol to its
  // mangled symbol. This function tries to find a mangled name
  // for U from the symbol table, and if found, set the symbol as
  // a weak alias for U.
  Symbol *findMangle(StringRef name);
  StringRef mangleMaybe(Symbol *s);

  // Symbol names are mangled by prepending "_" on x86.
  StringRef mangle(StringRef sym);

  // Windows specific -- "main" is not the only main function in Windows.
  // You can choose one from these four -- {w,}{WinMain,main}.
  // There are four different entry point functions for them,
  // {w,}{WinMain,main}CRTStartup, respectively. The linker needs to
  // choose the right one depending on which "main" function is defined.
  // This function looks up the symbol table and resolve corresponding
  // entry point name.
  StringRef findDefaultEntry();
  WindowsSubsystem inferSubsystem();

  // Build a set of COFF objects representing the combined contents of
  // BitcodeFiles and add them to the symbol table. Called after all files are
  // added and before the writer writes results to a file.
  void compileBitcodeFiles();

  void waitForLTOCleanup();

  // Creates an Undefined symbol and marks it as live.
  Symbol *addGCRoot(StringRef sym, bool aliasEC = false);
  Symbol *addGCRoot(Inserted s, llvm::CachedHashStringRef name);

  // Creates an Undefined symbol for a given name.
  Symbol *addUndefined(StringRef name);

  Symbol *addSynthetic(StringRef n, Chunk *c);
  Symbol *addAbsolute(StringRef n, uint64_t va);

  // The add* functions below that take a CachedHashStringRef are for callers
  // that hash the name ahead of time, e.g. object file parsing, which computes
  // the names and hashes of a batch of files in parallel before adding them.
  //
  // Each of them inserts the name and then resolves the new symbol against
  // what was there (a definition, a lazy symbol, ...). The overloads that
  // take an Inserted do the second half only: the batch replay in
  // LinkerDriver::addSegment() inserts all of a batch's names first, one
  // thread per shard, and resolves them in a second pass. They must be given
  // what insert() returned for the name, and the same name.
  Symbol *addUndefined(StringRef name, InputFile *f, bool overrideLazy) {
    return addUndefined(llvm::CachedHashStringRef(name), f, overrideLazy);
  }
  Symbol *addUndefined(llvm::CachedHashStringRef name, InputFile *f,
                       bool overrideLazy) {
    return addUndefined(insert(name, f), name, overrideLazy);
  }
  Symbol *addUndefined(Inserted s, llvm::CachedHashStringRef name,
                       bool overrideLazy);
  void addLazyArchive(ArchiveFile *f, const Archive::Symbol &sym);
  void addLazyArchive(Inserted s, ArchiveFile *f, const Archive::Symbol &sym);
  void addLazyObject(InputFile *f, StringRef n);
  void addLazyDLLSymbol(DLLFile *f, DLLFile::Symbol *sym, StringRef n);
  Symbol *addAbsolute(llvm::CachedHashStringRef n, COFFSymbolRef s) {
    return addAbsolute(insert(n, nullptr), n, s);
  }
  Symbol *addAbsolute(Inserted s, llvm::CachedHashStringRef n, COFFSymbolRef);
  Symbol *addRegular(InputFile *f, StringRef n,
                     const llvm::object::coff_symbol_generic *s = nullptr,
                     SectionChunk *c = nullptr, uint32_t sectionOffset = 0,
                     bool isWeak = false) {
    return addRegular(f, llvm::CachedHashStringRef(n), s, c, sectionOffset,
                      isWeak);
  }
  Symbol *addRegular(InputFile *f, llvm::CachedHashStringRef n,
                     const llvm::object::coff_symbol_generic *s = nullptr,
                     SectionChunk *c = nullptr, uint32_t sectionOffset = 0,
                     bool isWeak = false) {
    return addRegular(insert(n, f), f, n, s, c, sectionOffset, isWeak);
  }
  Symbol *addRegular(Inserted s, InputFile *f, llvm::CachedHashStringRef n,
                     const llvm::object::coff_symbol_generic *sym,
                     SectionChunk *c, uint32_t sectionOffset, bool isWeak);
  std::pair<DefinedRegular *, bool>
  addComdat(InputFile *f, StringRef n,
            const llvm::object::coff_symbol_generic *s = nullptr) {
    return addComdat(f, llvm::CachedHashStringRef(n), s);
  }
  std::pair<DefinedRegular *, bool>
  addComdat(InputFile *f, llvm::CachedHashStringRef n,
            const llvm::object::coff_symbol_generic *s = nullptr) {
    return addComdat(insert(n, f), f, n, s);
  }
  std::pair<DefinedRegular *, bool>
  addComdat(Inserted s, InputFile *f, llvm::CachedHashStringRef n,
            const llvm::object::coff_symbol_generic *sym);
  Symbol *addCommon(InputFile *f, StringRef n, uint64_t size,
                    const llvm::object::coff_symbol_generic *s = nullptr,
                    CommonChunk *c = nullptr) {
    return addCommon(f, llvm::CachedHashStringRef(n), size, s, c);
  }
  Symbol *addCommon(InputFile *f, llvm::CachedHashStringRef n, uint64_t size,
                    const llvm::object::coff_symbol_generic *s = nullptr,
                    CommonChunk *c = nullptr) {
    return addCommon(insert(n, f), f, n, size, s, c);
  }
  Symbol *addCommon(Inserted s, InputFile *f, llvm::CachedHashStringRef n,
                    uint64_t size, const llvm::object::coff_symbol_generic *sym,
                    CommonChunk *c);
  DefinedImportData *addImportData(StringRef n, ImportFile *f,
                                   Chunk *&location) {
    return addImportData(insert(n, nullptr), n, f, location);
  }
  DefinedImportData *addImportData(Inserted s, StringRef n, ImportFile *f,
                                   Chunk *&location);
  Defined *addImportThunk(StringRef name, DefinedImportData *s,
                          ImportThunkChunk *chunk) {
    return addImportThunk(insert(name, nullptr), name, s, chunk);
  }
  Defined *addImportThunk(Inserted s, StringRef name, DefinedImportData *id,
                          ImportThunkChunk *chunk);
  // Removes the symbol for `name` if it is one insert() created for
  // something that then did not happen (the batch replay inserts first and
  // resolves afterwards; an event that ends up not using its symbol leaves
  // it unresolved for a later event of the same name, or for this).
  void eraseIfUnresolved(llvm::CachedHashStringRef name);
  void addLibcall(StringRef name);
  void addEntryThunk(Symbol *from, Symbol *to);
  void addExitThunk(Symbol *from, Symbol *to);
  void initializeECThunks();
  void initializeSameAddressThunks();

  void reportDuplicate(Symbol *existing, InputFile *newFile,
                       SectionChunk *newSc = nullptr,
                       uint32_t newSectionOffset = 0);

  // While a batch of object files is being added
  // (COFFLinkerContext::deferDuplicateDiagnostics()), duplicate symbol
  // diagnostics are only recorded, because they describe the symbols'
  // sections, whose chunks are constructed after the batch's symbols are in
  // (see ObjFile::parseFinish()). This reports them, in the order they were
  // found.
  void reportDeferredDuplicates();

  COFFLinkerContext &ctx;
  llvm::COFF::MachineTypes machine;

  bool isEC() const { return machine == ARM64EC; }

  // An entry point symbol.
  Symbol *entry = nullptr;

  // A list of chunks which to be added to .rdata.
  std::vector<Chunk *> localImportChunks;

  // A list of EC EXP+ symbols.
  std::vector<Symbol *> expSymbols;

  std::vector<SameAddressThunkARM64EC *> sameAddressThunks;

  // A list of DLL exports.
  std::vector<Export> exports;
  llvm::DenseSet<StringRef> directivesExports;
  bool hadExplicitExports;

  Chunk *edataStart = nullptr;
  Chunk *edataEnd = nullptr;

  Symbol *delayLoadHelper = nullptr;
  Chunk *tailMergeUnwindInfoChunk = nullptr;

  // A list of wrapped symbols.
  std::vector<WrappedSymbol> wrapped;

  // Used for /alternatename.
  std::map<StringRef, StringRef> alternateNames;

  // Used for /aligncomm.
  std::map<std::string, int> alignComm;

  void fixupExports();
  void assignExportOrdinals();
  void parseModuleDefs(StringRef path);
  void parseAlternateName(StringRef);
  void parseAligncomm(StringRef);

  // Iterates symbols in non-determinstic hash table order.
  template <typename T> void forEachSymbol(T callback) {
    for (Shard &shard : shards)
      for (auto &pair : shard.map)
        callback(pair.second);
  }

  // The symbol map is split into shards by name hash, so that a batch of
  // files' symbols can be added with one thread per shard, each owning its
  // shard for the duration; see LinkerDriver::addSegment(). Names in a shard
  // share their top bits, which DenseMap does not use for bucketing. There
  // is one shard per thread of the pool (set by the driver once /threads is
  // known): the replay of a shard can wait for another shard's, so all of
  // them must run at once.
  unsigned numShards = 1;
  unsigned shardOf(llvm::CachedHashStringRef name) const {
    static_assert(numSymbolShards == 64, "shardOf() takes the top 6 bits");
    return ((name.hash() >> 26) * numShards) >> 6;
  }

  // State of a thread applying one shard's events of a batch: the key of the
  // event being applied (its position in the input, see addSegment()), and
  // what the event did that has to happen in input order and so is only
  // recorded here and done afterwards by the driver. Null on threads not
  // doing that.
  struct Pull {
    uint64_t key;
    ArchiveFile *file;
    Archive::Symbol sym;
  };
  struct DeferredDuplicate;
  struct ReplayContext {
    uint64_t key = 0;
    std::vector<Pull> pulls;
    std::vector<std::pair<uint64_t, Symbol *>> gcRoots;
    std::vector<DeferredDuplicate> duplicates;
    // Names whose event did not use the inserted symbol, see
    // eraseIfUnresolved().
    std::vector<llvm::CachedHashStringRef> maybeUnresolved;
  };
  static ReplayContext *replayContext();
  static void setReplayContext(ReplayContext *rc);
  // The key given to what is recorded outside a replay while a batch is
  // being added (set by the driver), so that it sorts with the replayed
  // events.
  uint64_t currentKey = 0;
  // Merges what a replay recorded into the deferred diagnostics.
  void addDeferredDuplicates(std::vector<DeferredDuplicate> &dups);

  // Set once a lazy object file or DLL symbol has been added: resolving an
  // undefined symbol against those parses the file right there, which the
  // batch replay cannot do, so it is not used from then on.
  bool hasLazyObjects = false;

  std::vector<BitcodeFile *> bitcodeFileInstances;

  DefinedRegular *loadConfigSym = nullptr;
  uint32_t loadConfigSize = 0;
  void initializeLoadConfig();

  std::string printSymbol(Symbol *sym) const;

private:
  /// Given a name without "__imp_" prefix, returns a defined symbol
  /// with the "__imp_" prefix, if it exists.
  Defined *impSymbol(StringRef name);
public:
  /// Inserts symbol if not already present.
  Inserted insert(llvm::CachedHashStringRef name);
  Inserted insert(StringRef name) {
    return insert(llvm::CachedHashStringRef(name));
  }
  /// Same as insert(Name), but also sets isUsedInRegularObj.
  Inserted insert(llvm::CachedHashStringRef name, InputFile *f);
  Inserted insert(StringRef name, InputFile *f) {
    return insert(llvm::CachedHashStringRef(name), f);
  }

  // See deferDuplicateDiagnostics(). `existing` holds a copy of the symbol as
  // it was when the duplicate was found (a SymbolUnion; the type is not
  // visible here).
  struct DeferredDuplicate {
    uint64_t key;
    alignas(16) char existing[96];
    InputFile *newFile;
    SectionChunk *newSc;
    uint32_t newSectionOffset;
  };

private:

  bool findUnderscoreMangle(StringRef sym);
  std::vector<Symbol *> getSymsWithPrefix(StringRef prefix);

  struct Shard {
    llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> map;
  };
  Shard shards[numSymbolShards];

  std::vector<DeferredDuplicate> deferredDuplicates;

public:
  // Pulls the archive member defining `sym` into the link -- or records
  // that, during a batch replay, for the driver to do in input order.
  void pullArchiveMember(ArchiveFile *f, const Archive::Symbol &sym);
  void printDuplicate(Symbol *existing, InputFile *newFile, SectionChunk *newSc,
                      uint32_t newSectionOffset);
  std::unique_ptr<BitcodeCompiler> lto;
  std::vector<std::pair<Symbol *, Symbol *>> entryThunks;
  llvm::DenseMap<Symbol *, Symbol *> exitThunks;

  void
  reportProblemSymbols(const llvm::SmallPtrSetImpl<Symbol *> &undefs,
                       const llvm::DenseMap<Symbol *, Symbol *> *localImports,
                       bool needBitcodeFiles);
  void reportUndefinedSymbol(const UndefinedDiag &undefDiag);
};

std::vector<std::string> getSymbolLocations(ObjFile *file, uint32_t symIndex);

StringRef ltrim1(StringRef s, const char *chars);

} // namespace lld::coff

#endif
