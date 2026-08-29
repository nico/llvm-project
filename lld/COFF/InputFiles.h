//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INPUT_FILES_H
#define LLD_COFF_INPUT_FILES_H

#include "Config.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/StringSaver.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace llvm {
struct DILineInfo;
namespace pdb {
class DbiModuleDescriptorBuilder;
class NativeSession;
}
namespace lto {
class InputFile;
}
}

namespace lld {
class DWARFCache;

namespace coff {
class COFFLinkerContext;

const COFFSyncStream &operator<<(const COFFSyncStream &, const InputFile *);

std::vector<MemoryBufferRef> getArchiveMembers(COFFLinkerContext &,
                                               llvm::object::Archive *file);

using llvm::COFF::IMAGE_FILE_MACHINE_UNKNOWN;
using llvm::COFF::MachineTypes;
using llvm::object::Archive;
using llvm::object::COFFObjectFile;
using llvm::object::COFFSymbolRef;
using llvm::object::coff_import_header;
using llvm::object::coff_section;

class Chunk;
class CommonChunk;
class Defined;
class DefinedImportData;
class DefinedImportThunk;
class DefinedRegular;
class ImportThunkChunk;
class ImportThunkChunkARM64EC;
class SectionChunk;
class Symbol;
class SymbolTable;
class Undefined;
class TpiSource;

// The contents of a .drectve section, tokenized and parsed as command line
// options (ArgParser::parseDirectives()). /EXPORT, /INCLUDE and
// /EXCLUDE-SYMBOLS are split off, as they can appear for potentially every
// symbol of an object and are handled in bulk.
struct ParsedDirectives {
  std::vector<StringRef> exports;
  std::vector<StringRef> includes;
  std::vector<StringRef> excludes;
  llvm::opt::InputArgList args;
};

// What SymbolTable::insert() returns: the symbol for the name, and whether
// the name was new.
struct Inserted {
  Symbol *sym;
  bool wasInserted;
};

// The symbol table is split into this many shards by name hash, see
// SymbolTable::shardOf().
constexpr unsigned numSymbolShards = 64;

// The indices of a file's symbol table events grouped by shard, in event
// order within a shard, for the two passes of LinkerDriver::addSegment().
class EventShards {
public:
  // `shardOfEvent[i]` is the shard of event i.
  void build(ArrayRef<uint8_t> shardOfEvent);
  ArrayRef<uint32_t> operator[](unsigned shard) const {
    return ArrayRef(order).slice(offsets[shard],
                                 offsets[shard + 1] - offsets[shard]);
  }

private:
  std::vector<uint32_t> order;
  uint32_t offsets[numSymbolShards + 1] = {};
};

// The root class of input files.
class InputFile {
public:
  enum Kind {
    ArchiveKind,
    ObjectKind,
    PDBKind,
    ImportKind,
    BitcodeKind,
    DLLKind
  };
  Kind kind() const { return fileKind; }
  virtual ~InputFile() {}

  // Returns the filename.
  StringRef getName() const { return mb.getBufferIdentifier(); }

  // Reads a file (the constructor doesn't do that).
  virtual void parse() = 0;

  // Returns the CPU type this file was compiled to.
  virtual MachineTypes getMachineType() const {
    return IMAGE_FILE_MACHINE_UNKNOWN;
  }

  MemoryBufferRef mb;

  // An archive file name if this file is created from an archive.
  StringRef parentName;

  // Returns .drectve section contents if exist.
  StringRef getDirectives() { return directives; }

  // The directives, parsed ahead of time (ObjFile::parsePrepare() does that
  // in parallel), if they were.
  std::unique_ptr<ParsedDirectives> parsedDirectives;

  // For LinkerDriver::addSegment(), which adds the symbols of a run of files
  // with one thread per shard of the symbol table: whether this file's symbol
  // table half can be done that way (it was prepared, and nothing about it
  // needs the one-file-at-a-time path), and the two passes over one shard's
  // events -- inserting their names, then resolving them, with `fileKey` (the
  // file's position) forming the events' keys. See SymbolTable::Inserted.
  virtual bool shardable() const { return false; }
  virtual void insertShardEvents(unsigned shard) {}
  virtual void applyShardEvents(unsigned shard, uint64_t fileKey) {}
  // Called once, in input order, after all of a file's events were applied.
  virtual void afterSymbols() {}

  SymbolTable &symtab;

protected:
  InputFile(SymbolTable &s, Kind k, MemoryBufferRef m, bool lazy = false)
      : mb(m), symtab(s), fileKind(k), lazy(lazy) {}

  StringRef directives;

private:
  const Kind fileKind;

public:
  // True if this is a lazy ObjFile or BitcodeFile.
  bool lazy = false;
};

// .lib or .a file.
class ArchiveFile : public InputFile {
public:
  explicit ArchiveFile(COFFLinkerContext &ctx, MemoryBufferRef mb,
                       std::unique_ptr<Archive> &f);
  static bool classof(const InputFile *f) { return f->kind() == ArchiveKind; }
  void parse() override;
  // Reads and hashes the names in the archive's symbol index, so that parse()
  // only has the symbol table to talk to. Thread-safe.
  void parsePrepare();

  bool shardable() const override { return prepared && !needsSerialParse; }
  void insertShardEvents(unsigned shard) override;
  void applyShardEvents(unsigned shard, uint64_t fileKey) override;
  void afterSymbols() override;

  // Enqueues an archive member load for the given symbol. If we've already
  // enqueued a load for the same archive member, this function does nothing,
  // which ensures that we don't load the same member more than once.
  void addMember(const Archive::Symbol &sym);

private:
  std::unique_ptr<Archive> file;
  llvm::DenseSet<uint64_t> seen;

  // One per symbol in the index, from parsePrepare(); valid until parsed.
  struct Event {
    llvm::CachedHashStringRef name;
    Archive::Symbol sym;
    Inserted slot;
  };
  std::vector<Event> events;
  EventShards eventShards;
  bool prepared = false;
  // Set if the index has a DllMain symbol, whose handling looks at the member
  // and keeps per-archive state, which the sharded replay does not do.
  bool needsSerialParse = false;
};

// .obj or .o file. This may be a member of an archive file.
class ObjFile : public InputFile {
public:
  static ObjFile *create(COFFLinkerContext &ctx, COFFObjectFile *coffObj,
                         bool lazy = false);
  static ObjFile *create(COFFLinkerContext &ctx, MemoryBufferRef mb,
                         bool lazy = false) {
    return ObjFile::create(ctx, ObjFile::createCOFFObject(ctx, mb).release(),
                           lazy);
  }
  explicit ObjFile(SymbolTable &symtab, COFFObjectFile *coffObj, bool lazy);

  static std::unique_ptr<COFFObjectFile>
  createCOFFObject(COFFLinkerContext &ctx, MemoryBufferRef mb);

  static bool classof(const InputFile *f) { return f->kind() == ObjectKind; }
  // Parsing is in three steps, so that a batch of files can do the first and
  // last in parallel and only the middle one, which touches the symbol table,
  // one file at a time (that is what parse() does for a single file):
  //
  //  parsePrepare(): everything that depends on this file alone -- the
  //    chunks of the non-COMDAT sections, the local symbols in them, common
  //    chunks, the names and hashes of every symbol the symbol table will be
  //    asked about, the flags from .debug$S -- and the list of what the two
  //    other steps do, in order.
  //  parseSymbols(): the symbol table half -- undefined, absolute, common and
  //    regular symbols, COMDAT leaders and which of them prevail. This walks
  //    the symbol table in input order, as the linker always did, so the
  //    resolution is the same. It only allocates the chunks of the prevailing
  //    COMDAT sections, so that the symbols can point at them, and leaves
  //    constructing them to
  //  parseFinish(): the chunks of the sections that turned out to prevail,
  //    the local symbols in them, associative sections.
  //
  // Anything a step needs from an earlier one is done by that step if it has
  // not been done, so calling parseSymbols() on an unprepared file works.
  void parse() override;
  void parsePrepare();
  void parseSymbols();
  void parseFinish();
  // What parseSymbols() does after the symbol table half: the type source
  // dependencies (in input order, so a caller that replays the symbol table
  // half by shard does this per file afterwards) and, on ARM64EC, the thunks.
  void afterSymbols() override;

  bool shardable() const override;
  void insertShardEvents(unsigned shard) override;
  void applyShardEvents(unsigned shard, uint64_t fileKey) override;
  // Adds the GC roots from the directives (/include, /entry) as events for
  // the sharded replay; the entry point, if any, is stored in `entrySymbol`
  // once they are applied. The one-file-at-a-time path handles them in
  // LinkerDriver::parseDirectives() instead.
  void addRootEvents();
  Symbol *entrySymbol = nullptr;
  bool rootsAsEvents = false;
  // Adds the sections parseFinish() found to be subject to string tail
  // merging to their MergeChunks. Not thread-safe; called in input order.
  void addTailMergeSections();
  void parseLazy();
  MachineTypes getMachineType() const override;
  ArrayRef<Chunk *> getChunks() { return chunks; }
  ArrayRef<SectionChunk *> getDebugChunks() { return debugChunks; }
  ArrayRef<SectionChunk *> getSXDataChunks() { return sxDataChunks; }
  ArrayRef<SectionChunk *> getGuardFidChunks() { return guardFidChunks; }
  ArrayRef<SectionChunk *> getGuardIATChunks() { return guardIATChunks; }
  ArrayRef<SectionChunk *> getGuardLJmpChunks() { return guardLJmpChunks; }
  ArrayRef<SectionChunk *> getGuardEHContChunks() { return guardEHContChunks; }
  ArrayRef<Symbol *> getSymbols() { return symbols; }

  MutableArrayRef<Symbol *> getMutableSymbols() { return symbols; }

  ArrayRef<uint8_t> getDebugSection(StringRef secName);

  // Returns a Symbol object for the symbolIndex'th symbol in the
  // underlying object file.
  Symbol *getSymbol(uint32_t symbolIndex) {
    return symbols[symbolIndex];
  }

  // Returns the underlying COFF file.
  COFFObjectFile *getCOFFObj() { return coffObj.get(); }

  // Add a symbol for a range extension thunk. Return the new symbol table
  // index. This index can be used to modify a relocation.
  uint32_t addRangeThunkSymbol(Symbol *thunk) {
    symbols.push_back(thunk);
    return symbols.size() - 1;
  }

  void includeResourceChunks();

  bool isResourceObjFile() const { return !resourceChunks.empty(); }

  // Flags in the absolute @feat.00 symbol if it is present. These usually
  // indicate if an object was compiled with certain security features enabled
  // like stack guard, safeseh, /guard:cf, or other things.
  uint32_t feat00Flags = 0;

  // True if this object file is compatible with SEH.  COFF-specific and
  // x86-only. COFF spec 5.10.1. The .sxdata section.
  bool hasSafeSEH() { return feat00Flags & 0x1; }

  // True if this file was compiled with /guard:cf.
  bool hasGuardCF() { return feat00Flags & 0x800; }

  // True if this file was compiled with /guard:ehcont.
  bool hasGuardEHCont() { return feat00Flags & 0x4000; }

  // Pointer to the PDB module descriptor builder. Various debug info records
  // will reference object files by "module index", which is here. Things like
  // source files and section contributions are also recorded here. Will be null
  // if we are not producing a PDB.
  llvm::pdb::DbiModuleDescriptorBuilder *moduleDBI = nullptr;

  const coff_section *addrsigSec = nullptr;

  const coff_section *callgraphSec = nullptr;

  // When using Microsoft precompiled headers, this is the PCH's key.
  // The same key is used by both the precompiled object, and objects using the
  // precompiled object. Any difference indicates out-of-date objects.
  std::optional<uint32_t> pchSignature;

  // Whether this file was compiled with /hotpatch.
  bool hotPatchable = false;

  // Whether the object was already merged into the final PDB.
  bool mergedIntoPDB = false;

  // If the OBJ has a .debug$T stream, this tells how it will be handled.
  TpiSource *debugTypesObj = nullptr;

  // The .debug$P or .debug$T section data if present. Empty otherwise.
  ArrayRef<uint8_t> debugTypes;

  std::optional<std::pair<StringRef, uint32_t>>
  getVariableLocation(StringRef var);

  std::optional<llvm::DILineInfo> getDILineInfo(uint32_t offset,
                                                uint32_t sectionIndex);

private:
  const coff_section* getSection(uint32_t i);
  const coff_section *getSection(COFFSymbolRef sym) {
    return getSection(sym.getSectionNumber());
  }

  void enqueuePdbFile(StringRef path, ObjFile *fromFile);

  void initializeChunks();
  void prepareSymbols();
  void initializeFlags();
  void initializeDependencies();
  void initializeECThunks();

  // Creates the chunk for a section, or constructs it into `storage` if that
  // was allocated by allocateChunk(). Returns null for sections the linker
  // consumes or drops (see sectionHasChunk()).
  SectionChunk *
  readSection(uint32_t sectionNumber,
              const llvm::object::coff_aux_section_definition *def,
              StringRef leaderName, SectionChunk *storage = nullptr);
  bool sectionHasChunk(const coff_section *sec, StringRef name);
  SectionChunk *allocateChunk();

  void recordPrevailingSymbolForMingw(
      COFFSymbolRef coffSym,
      llvm::DenseMap<StringRef, uint32_t> &prevailingSectionMap);

  void maybeAssociateSEHForMingw(
      COFFSymbolRef sym, const llvm::object::coff_aux_section_definition *def,
      const llvm::DenseMap<StringRef, uint32_t> &prevailingSectionMap);

  // Given a new symbol Sym with comdat selection Selection, if the new
  // symbol is not (yet) Prevailing and the existing comdat leader set to
  // Leader, emits a diagnostic if the new symbol and its selection doesn't
  // match the existing symbol and its selection. If either old or new
  // symbol have selection IMAGE_COMDAT_SELECT_LARGEST, Sym might replace
  // the existing leader. In that case, Prevailing is set to true.
  void
  handleComdatSelection(COFFSymbolRef sym, llvm::COFF::COMDATType &selection,
                        bool &prevailing, DefinedRegular *leader,
                        const llvm::object::coff_aux_section_definition *def);

  // What parseSymbols() does for one symbol, recorded by parsePrepare() in
  // the order the symbols are added to the symbol table. Events that decide
  // what happens to a section (whether a COMDAT leader prevails, whether an
  // associative section's parent did) publish that with decideSection(), and
  // events that need it wait for it (waitDecided()): with the sharded replay,
  // the two can be on different threads.
  struct SymbolEvent {
    enum Kind : uint8_t {
      Undefined,    // addUndefined; `flag` is overrideLazy
      Common,       // addCommon with `chunk`
      Absolute,     // addAbsolute
      ComdatLeader, // addComdat for the leader of `section`, with `def`
      Regular,      // addRegular in `section`, or addUndefined if discarded
      WeakAlias,    // make the symbol a weak alias of `target`; `flag` is
                    // antiDep
      Associative,  // decide the associative `section` (with `def`) from its
                    // parent's fate, and allocate its chunk
      AssociativeInvalid, // an associative section whose parent is not
                          // resolved by then: diagnose
      MingwPrevailing, // MinGW: note the section if its leader prevailed
      MingwSEH,        // MinGW: associate a .[px]data$func section
      GCRoot,          // addGCRoot (from /include or, `flag`, /entry)
    };
    Kind kind;
    bool flag = false;
    uint8_t shard = 0;
    uint32_t index;
    union {
      uint32_t section;
      uint32_t target;
    };
    llvm::CachedHashStringRef name;
    union {
      CommonChunk *chunk;
      const llvm::object::coff_aux_section_definition *def;
    };
    // From the first pass of the sharded replay, for the second.
    Inserted slot = {nullptr, false};
  };
  // What parseFinish() does for one symbol, in order.
  struct FinishItem {
    enum Kind : uint8_t {
      PushChunk,     // append `chunk` (a common or empty section) to chunks
      ComdatSection, // construct the section of the COMDAT leader (with the
                     // section definition `def`), if it prevailed
      LocalSymbol,   // create the local symbol, if its section was kept
      Associative,   // construct the associative section (with `def`), if
                     // parseSymbols() decided it is read
      MingwSEH,      // MinGW: the same for a section it associated
    };
    Kind kind;
    uint32_t index;
    Chunk *chunk = nullptr;
    const llvm::object::coff_aux_section_definition *def = nullptr;
    // LocalSymbol: the section was still pending when the symbol was seen.
    bool pending = false;
  };

  void insertEvent(SymbolEvent &e);
  void applyEvent(SymbolEvent &e);
  void decideSection(uint32_t sectionNumber);
  void waitDecided(uint32_t sectionNumber);
  void constructAssociative(
      uint32_t sectionNumber,
      const llvm::object::coff_aux_section_definition *def,
      uint32_t parentIndex);

  // Per-section state of the walk in prepareSymbols().
  struct PrepareState;
  void prepareDefined(COFFSymbolRef sym, uint32_t index, PrepareState &state);
  Symbol *addComdatLeader(COFFSymbolRef sym, const SymbolEvent &e);
  Symbol *createLocal(COFFSymbolRef sym);
  Symbol *createUndefined(Inserted in, COFFSymbolRef sym,
                          llvm::CachedHashStringRef name, bool overrideLazy);

  std::unique_ptr<COFFObjectFile> coffObj;

  // List of all chunks defined by this file. This includes both section
  // chunks and non-section chunks for common symbols.
  std::vector<Chunk *> chunks;

  std::vector<SectionChunk *> resourceChunks;

  // CodeView debug info sections.
  std::vector<SectionChunk *> debugChunks;

  // Chunks containing symbol table indices of exception handlers. Only used for
  // 32-bit x86.
  std::vector<SectionChunk *> sxDataChunks;

  // Chunks containing symbol table indices of address taken symbols, address
  // taken IAT entries, longjmp and ehcont targets. These are not linked into
  // the final binary when /guard:cf is set.
  std::vector<SectionChunk *> guardFidChunks;
  std::vector<SectionChunk *> guardIATChunks;
  std::vector<SectionChunk *> guardLJmpChunks;
  std::vector<SectionChunk *> guardEHContChunks;

  std::vector<SectionChunk *> hybmpChunks;

  // This vector contains a list of all symbols defined or referenced by this
  // file. They are indexed such that you can get a Symbol by symbol
  // index. Nonexistent indices (which are occupied by auxiliary
  // symbols in the real symbol table) are filled with null pointers.
  std::vector<Symbol *> symbols;

  // This vector contains the same chunks as Chunks, but they are
  // indexed such that you can get a SectionChunk by section index.
  // Nonexistent section indices are filled with null pointers.
  // (Because section number is 1-based, the first slot is always a
  // null pointer.) This vector is only valid during initialization.
  std::vector<SectionChunk *> sparseChunks;

  // Only valid during initialization, see parsePrepare().
  std::vector<SymbolEvent> symbolEvents;
  std::vector<SymbolEvent> rootEvents;
  EventShards eventShards, rootEventShards;
  std::vector<FinishItem> finishItems;
  // For every COMDAT section, whether readSection() makes a chunk for it.
  llvm::BitVector comdatSectionHasChunk;
  // For every section, whether its fate (sparseChunks) is known; see
  // SymbolEvent.
  std::unique_ptr<std::atomic<uint8_t>[]> sectionDecided;
  // The shard of the event that decides each COMDAT section, so that the
  // events of its associative sections can go to the same shard.
  std::vector<uint8_t> sectionShard;
  // A lock for the one thing two shards' events can both do to this file:
  // create the local symbol a weak alias points at.
  std::mutex localSymbolMutex;

  // The COMDAT selection of every section that has a prevailing leader. Kept
  // around because the selection of a leader's section is consulted when
  // another definition of the leader comes along, possibly before the chunk
  // has been constructed.
  std::vector<uint8_t> comdatSelections;

  // Sections subject to string tail merging, from parseFinish().
  std::vector<SectionChunk *> tailMergeSections;

  // MinGW: the .[px]data$func sections parseSymbols() associated with a
  // function's section, by section number, with the parent's; and the
  // prevailing function sections they can be associated with, by name.
  llvm::DenseMap<uint32_t, uint32_t> mingwSEHParents;
  llvm::DenseMap<StringRef, uint32_t> mingwPrevailingSectionMap;

  bool prepared = false;
  bool symbolsParsed = false;
  bool finished = false;
  bool flagsInitialized = false;

  DWARFCache *dwarf = nullptr;
};

// This is a PDB type server dependency, that is not a input file per se, but
// needs to be treated like one. Such files are discovered from the debug type
// stream.
class PDBInputFile : public InputFile {
public:
  explicit PDBInputFile(COFFLinkerContext &ctx, MemoryBufferRef m);
  ~PDBInputFile();
  static bool classof(const InputFile *f) { return f->kind() == PDBKind; }
  void parse() override;

  static PDBInputFile *findFromRecordPath(const COFFLinkerContext &ctx,
                                          StringRef path, ObjFile *fromFile);

  // Record possible errors while opening the PDB file
  std::optional<std::string> loadErrorStr;

  // This is the actual interface to the PDB (if it was opened successfully)
  std::unique_ptr<llvm::pdb::NativeSession> session;

  // If the PDB has a .debug$T stream, this tells how it will be handled.
  TpiSource *debugTypesObj = nullptr;
};

// This type represents import library members that contain DLL names
// and symbols exported from the DLLs. See Microsoft PE/COFF spec. 7
// for details about the format.
class ImportFile : public InputFile {
public:
  explicit ImportFile(COFFLinkerContext &ctx, MemoryBufferRef m);

  static bool classof(const InputFile *f) { return f->kind() == ImportKind; }
  MachineTypes getMachineType() const override { return getMachineType(mb); }
  static MachineTypes getMachineType(MemoryBufferRef m);
  bool isSameImport(const ImportFile *other) const;
  bool isEC() const { return impECSym != nullptr; }

  // Reads the header and works out the names; thread-safe. parse() then adds
  // the symbols.
  void parsePrepare();
  bool shardable() const override;
  void insertShardEvents(unsigned shard) override;
  void applyShardEvents(unsigned shard, uint64_t fileKey) override;
  void afterSymbols() override;

  DefinedImportData *impSym = nullptr;
  Defined *thunkSym = nullptr;
  ImportThunkChunkARM64EC *impchkThunk = nullptr;
  ImportFile *hybridFile = nullptr;
  std::string dllName;

private:
  void parse() override;
  ImportThunkChunk *makeImportThunk();

public:
  StringRef externalName;
  const coff_import_header *hdr;
  Chunk *location = nullptr;

  // Auxiliary IAT symbols and chunks on ARM64EC.
  DefinedImportData *impECSym = nullptr;
  Chunk *auxLocation = nullptr;
  Defined *auxThunkSym = nullptr;
  DefinedImportData *auxImpCopySym = nullptr;
  Chunk *auxCopyLocation = nullptr;

  // From parsePrepare(): the import's name and the derived names (owned
  // here, since symbols point at them), and what parse() adds, in order:
  // the __imp_ symbol, the data symbol of an IMPORT_CONST, the thunk.
  bool prepared = false;
  StringRef name;
  std::string ecName;
  std::string impName, auxImpName, auxImpCopyName, auxThunkName, impChkName;
  bool isCode = false;
  struct Event {
    enum Kind : uint8_t { ImportData, ConstData, Thunk };
    Kind kind;
    Inserted slot;
  };
  llvm::SmallVector<Event, 3> events;
  uint8_t eventShard[3] = {};
  // Whether the __imp_ symbol went in (the others depend on it): 0 while
  // undecided, 1 if it did, 2 if it was a duplicate.
  std::atomic<uint8_t> impSymState{0};

  // We want to eliminate dllimported symbols if no one actually refers to them.
  // These "Live" bits are used to keep track of which import library members
  // are actually in use.
  //
  // If the Live bit is turned off by MarkLive, Writer will ignore dllimported
  // symbols provided by this import library member.
  bool live;
};

// Used for LTO.
class BitcodeFile : public InputFile {
public:
  explicit BitcodeFile(SymbolTable &symtab, MemoryBufferRef mb,
                       std::unique_ptr<llvm::lto::InputFile> &obj, bool lazy);
  ~BitcodeFile();

  static BitcodeFile *create(COFFLinkerContext &ctx, MemoryBufferRef mb,
                             StringRef archiveName, uint64_t offsetInArchive,
                             bool lazy);
  static bool classof(const InputFile *f) { return f->kind() == BitcodeKind; }
  ArrayRef<Symbol *> getSymbols() { return symbols; }
  MachineTypes getMachineType() const override {
    return getMachineType(obj.get());
  }
  static MachineTypes getMachineType(const llvm::lto::InputFile *obj);
  void parseLazy();
  std::unique_ptr<llvm::lto::InputFile> obj;

private:
  void parse() override;

  std::vector<Symbol *> symbols;
};

// .dll file. MinGW only.
class DLLFile : public InputFile {
public:
  explicit DLLFile(SymbolTable &symtab, std::unique_ptr<COFFObjectFile> &obj)
      : InputFile(symtab, DLLKind, obj->getMemoryBufferRef()) {
    coffObj.swap(obj);
  }
  static bool classof(const InputFile *f) { return f->kind() == DLLKind; }
  void parse() override;
  MachineTypes getMachineType() const override;

  struct Symbol {
    StringRef dllName;
    StringRef symbolName;
    llvm::COFF::ImportNameType nameType;
    llvm::COFF::ImportType importType;
  };

  void makeImport(Symbol *s);

private:
  std::unique_ptr<COFFObjectFile> coffObj;
  llvm::StringSet<> seen;
};

inline bool isBitcode(MemoryBufferRef mb) {
  return identify_magic(mb.getBuffer()) == llvm::file_magic::bitcode;
}

std::string replaceThinLTOSuffix(StringRef path, StringRef suffix,
                                 StringRef repl);
} // namespace coff

std::string toString(const coff::InputFile *file);
} // namespace lld

#endif
