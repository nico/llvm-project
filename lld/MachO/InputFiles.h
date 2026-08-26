//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_INPUT_FILES_H
#define LLD_MACHO_INPUT_FILES_H

#include "MachOStructs.h"
#include "Target.h"

#include "lld/Common/DWARF.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Memory.h"
#include "llvm/Support/StringSaver.h"

#include <atomic>
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Threading.h"
#include "llvm/TextAPI/TextAPIReader.h"

#include <vector>

namespace llvm {
namespace lto {
class InputFile;
} // namespace lto
namespace MachO {
class InterfaceFile;
} // namespace MachO
class TarWriter;
} // namespace llvm

namespace lld {
namespace macho {

struct PlatformInfo;
class ConcatInputSection;
class Symbol;
class Defined;
class AliasSymbol;
struct Relocation;
enum class RefState : uint8_t;

// If --reproduce option is given, all input files are written
// to this tar archive.
extern std::unique_ptr<llvm::TarWriter> tar;

// If .subsections_via_symbols is set, each InputSection will be split along
// symbol boundaries. The field offset represents the offset of the subsection
// from the start of the original pre-split InputSection.
struct Subsection {
  uint64_t offset = 0;
  InputSection *isec = nullptr;
};

using Subsections = std::vector<Subsection>;
class InputFile;

class Section {
public:
  InputFile *file;
  StringRef segname;
  StringRef name;
  uint32_t flags;
  uint64_t addr;
  Subsections subsections;

  Section(InputFile *file, StringRef segname, StringRef name, uint32_t flags,
          uint64_t addr)
      : file(file), segname(segname), name(name), flags(flags), addr(addr) {}
  // Ensure pointers to Sections are never invalidated.
  Section(const Section &) = delete;
  Section &operator=(const Section &) = delete;
  Section(Section &&) = delete;
  Section &operator=(Section &&) = delete;

private:
  // Whether we have already split this section into individual subsections.
  // For sections that cannot be split (e.g. literal sections), this is always
  // false.
  bool doneSplitting = false;
  friend class ObjFile;
};

// Represents a call graph profile edge.
struct CallGraphEntry {
  // The index of the caller in the symbol table.
  uint32_t fromIndex;
  // The index of the callee in the symbol table.
  uint32_t toIndex;
  // Number of calls from callee to caller in the profile.
  uint64_t count;

  CallGraphEntry(uint32_t fromIndex, uint32_t toIndex, uint64_t count)
      : fromIndex(fromIndex), toIndex(toIndex), count(count) {}
};

class InputFile {
public:
  enum Kind {
    ObjKind,
    OpaqueKind,
    DylibKind,
    ArchiveKind,
    BitcodeKind,
  };

  virtual ~InputFile() = default;
  Kind kind() const { return fileKind; }
  StringRef getName() const { return name; }
  static void resetIdCount() { idCount = 0; }

  MemoryBufferRef mb;

  std::vector<Symbol *> symbols;
  std::vector<Section *> sections;
  ArrayRef<uint8_t> objCImageInfo;

  // The symbol table operations this file performs, see parseBatch(): the
  // indices of its symbol events, grouped by the symbol table shard the name
  // falls in, so that each shard can replay just its own. shardStart has one
  // more entry than there are shards.
  std::vector<uint32_t> shardOrder;
  std::vector<uint32_t> shardStart;

  // If not empty, this stores the name of the archive containing this file.
  // We use this string for creating error messages.
  std::string archiveName;

  // Provides an easy way to sort InputFiles deterministically.
  const int id;

  // True if this is a lazy ObjFile or BitcodeFile.
  bool lazy = false;

protected:
  InputFile(Kind kind, MemoryBufferRef mb, bool lazy = false)
      : mb(mb), id(idCount++), lazy(lazy), fileKind(kind),
        name(mb.getBufferIdentifier()) {}

  InputFile(Kind, const llvm::MachO::InterfaceFile &);

  // If true, this input's arch is compatible with target.
  bool compatArch = true;

private:
  const Kind fileKind;
  const StringRef name;

  static int idCount;
};

struct FDE {
  uint32_t funcLength;
  Symbol *personality;
  InputSection *lsda;
};

// .o file
class ObjFile final : public InputFile {
public:
  // With deferParse, the constructor does not parse the file; the caller is
  // expected to hand it to parseLater().
  ObjFile(MemoryBufferRef mb, uint32_t modTime, StringRef archiveName,
          bool lazy = false, bool forceHidden = false, bool compatArch = true,
          bool builtFromBitcode = false, bool deferParse = false);
  ArrayRef<llvm::MachO::data_in_code_entry> getDataInCode() const;
  ArrayRef<uint8_t> getOptimizationHints() const;
  template <class LP> void parse();
  // parse() split in two. parsePrepare() is the per-file half: it touches no
  // global state, so it can be run for many files at once. parseFinish() is
  // the half that inserts into the symbol table, and has to run in file order.
  template <class LP> void parsePrepare();
  template <class LP> void parseFinish();
  // parseLazy() split the same way: scanLazy() collects the names this file
  // defines, registerLazy() adds them to the symbol table.
  template <class LP> void scanLazy();
  void registerLazy();
  // The pieces of parseFinish() / registerLazy(), for replaying this file's
  // symbol events one shard at a time: the number of events, performing one,
  // registering the external symbols with their sections once all events of
  // all files have been performed, and the rest of parseFinish().
  size_t numSymbolEvents() const;
  llvm::CachedHashStringRef symbolEventName(size_t i) const;
  void replaySymbolEvent(size_t i);
  // `before`, if given, is called with the event index of each symbol right
  // before it is registered.
  // Adds the file's external defined symbols to their sections' symbol
  // lists (the local ones were added as they were created) -- except those
  // of the sections in `holdBack`, which are handed to `heldBack` with their
  // event index instead.
  void registerSymbolsWithSections(
      const llvm::DenseSet<InputSection *> &holdBack,
      llvm::function_ref<void(size_t, Defined *)> heldBack);
  size_t firstDefinedEvent() const { return nonSectionSymbols.size(); }
  void clearSymbolEvents();
  template <class LP> void finishParse();
  template <class LP>
  void parseLinkerOptions(llvm::SmallVectorImpl<StringRef> &LinkerOptions);
  // Parses the relocations that parse() left for later. See
  // macho::parseDeferredRelocations().
  void parseDeferredRelocations();

  static bool classof(const InputFile *f) { return f->kind() == ObjKind; }

  // The compile unit's source file, from the debug info. Computed by
  // parsePrepare(), since finding it parses the unit's DIE.
  std::string sourceFile() const;
  static std::string sourceFileOf(llvm::DWARFUnit *compileUnit);
  // Parses line table information for diagnostics. compileUnit should be used
  // for other purposes.
  lld::DWARFCache *getDwarf();

  // Set by finishParse(); until then, the unit parsePrepare() found is only
  // in parsedCompileUnit. That keeps the diagnostics that symbol resolution
  // may produce (duplicate symbols) from reading debug info from several
  // threads at once.
  llvm::DWARFUnit *compileUnit = nullptr;
  llvm::DWARFUnit *parsedCompileUnit = nullptr;
  std::string cachedSourceFile;
  std::unique_ptr<lld::DWARFCache> dwarfCache;
  Section *addrSigSection = nullptr;
  const uint32_t modTime;
  bool forceHidden;
  bool builtFromBitcode;
  std::vector<ConcatInputSection *> debugSections;
  std::vector<CallGraphEntry> callGraph;
  llvm::DenseMap<ConcatInputSection *, FDE> fdes;
  std::vector<AliasSymbol *> aliases;

private:
  llvm::once_flag initDwarf;
  template <class LP> void parseLazy();
  template <class SectionHeader> void parseSections(ArrayRef<SectionHeader>);
  template <class LP>
  void parseSymbolsPrepare(ArrayRef<typename LP::section> sectionHeaders,
                           ArrayRef<typename LP::nlist> nList,
                           const char *strtab, bool subsectionsViaSymbols);

  // Filled in by parsePrepare(), consumed by parseFinish(). The symbols that
  // go through the symbol table are recorded with their name already hashed,
  // so that the half of parsing that runs one file at a time has nothing left
  // to do with the name but look it up.
  llvm::SmallVector<StringRef, 4> lcLinkerOptions;
  std::vector<std::vector<uint32_t>> symbolsBySection;
  struct PendingSymbol {
    llvm::CachedHashStringRef name;
    uint32_t symIndex;
    // The nlist fields the symbol table operation needs.
    uint64_t n_value;
    uint16_t n_desc;
    uint8_t n_type;
  };
  std::vector<PendingSymbol> undefineds;
  std::vector<PendingSymbol> nonSectionSymbols;
  // Filled in by scanLazy(), consumed by registerLazy().
  std::vector<PendingSymbol> lazyDefineds;
  // An external section symbol, and the subsection it was found to be in.
  struct PendingDefined {
    llvm::CachedHashStringRef name;
    uint32_t symIndex;
    InputSection *isec;
    uint64_t value;
    uint64_t size;
    uint16_t n_desc;
    uint8_t n_type;
  };
  std::vector<PendingDefined> pendingDefineds;
  Symbol *parseNonSectionSymbol(const PendingSymbol &sym);
  Symbol *createExternalDefined(const PendingDefined &sym);
  template <class NList>
  Symbol *createLocalNonSectionSymbol(const NList &sym, const char *strtab);
  template <class LP> void parseSymbols();
  template <class SectionHeader>
  void parseRelocations(ArrayRef<SectionHeader> sectionHeaders,
                        const SectionHeader &, Section &);
  template <class LP> void parseDeferredRelocationsImpl();
  void parseDebugInfo();
  void splitEhFrames(ArrayRef<uint8_t> dataArr, Section &ehFrameSection);
  void registerCompactUnwind(Section &compactUnwindSection);
  void registerEhFrames(Section &ehFrameSection);
};

// command-line -sectcreate file
class OpaqueFile final : public InputFile {
public:
  OpaqueFile(MemoryBufferRef mb, StringRef segName, StringRef sectName);
  static bool classof(const InputFile *f) { return f->kind() == OpaqueKind; }
};

// .dylib or .tbd file
class DylibFile final : public InputFile {
public:
  // Mach-O dylibs can re-export other dylibs as sub-libraries, meaning that the
  // symbols in those sub-libraries will be available under the umbrella
  // library's namespace. Those sub-libraries can also have their own
  // re-exports. When loading a re-exported dylib, `umbrella` should be set to
  // the root dylib to ensure symbols in the child library are correctly bound
  // to the root. On the other hand, if a dylib is being directly loaded
  // (through an -lfoo flag), then `umbrella` should be a nullptr.
  explicit DylibFile(MemoryBufferRef mb, DylibFile *umbrella,
                     bool isBundleLoader, bool explicitlyLinked);
  explicit DylibFile(const llvm::MachO::InterfaceFile &interface,
                     DylibFile *umbrella, bool isBundleLoader,
                     bool explicitlyLinked);
  explicit DylibFile(DylibFile *umbrella);

  void parseLoadCommands(MemoryBufferRef mb);
  void parseReexports(const llvm::MachO::InterfaceFile &interface);
  // Export processing is split like ObjFile's parsing. scanExports() walks
  // the export trie, or filters and sorts the TBD's symbol list; it touches
  // nothing but this file, so it can run for many files at once (from the
  // batch the file was put on with parseLater()). recordExports() then
  // handles the $ld$ symbols and records the exports; that has to happen in
  // file order, since $ld$hide symbols change the umbrella's hidden set for
  // the dylibs after them. registerExports() adds them to the symbol table,
  // and replayExport() does one of them.
  void scanExports();
  // The two halves of finishing the exports, see parseBatch(): the $ld$
  // symbols, which can hide symbols of this and of other files and so go
  // in file order, and the recording of the rest, which is per file.
  void handleLDSymbols();
  void recordExports();
  // The file's position in parse order, set by parseBatch().
  uint32_t parseOrder = 0;
  void registerExports();
  size_t numExports() const { return pendingExports.size(); }
  llvm::CachedHashStringRef exportName(size_t i) const {
    return pendingExports[i].name;
  }
  void replayExport(size_t i);
  void clearExports() {
    pendingExports = {};
    shardOrder = {};
    shardStart = {};
  }
  bool isReferenced() const { return numReferencedSymbols > 0; }
  bool isExplicitlyLinked() const;
  void setExplicitlyLinked() { explicitlyLinked = true; }

  static bool classof(const InputFile *f) { return f->kind() == DylibKind; }

  StringRef installName;
  DylibFile *exportingFile = nullptr;
  DylibFile *umbrella;
  SmallVector<StringRef, 2> rpaths;
  SmallVector<StringRef> allowableClients;
  uint32_t compatibilityVersion = 0;
  uint32_t currentVersion = 0;
  int64_t ordinal = 0; // Ordinal numbering starts from 1, so 0 is a sentinel
  // Symbols of several files can be added at once, see parseBatch().
  std::atomic<unsigned> numReferencedSymbols{0};
  RefState refState;
  bool reexport = false;
  bool forceNeeded = false;
  bool forceWeakImport = false;
  bool deadStrippable = false;

private:
  bool explicitlyLinked = false; // Access via isExplicitlyLinked().

public:
  // An executable can be used as a bundle loader that will load the output
  // file being linked, and that contains symbols referenced, but not
  // implemented in the bundle. When used like this, it is very similar
  // to a dylib, so we've used the same class to represent it.
  bool isBundleLoader;

  // Synthetic Dylib objects created by $ld$previous symbols in this dylib.
  // Usually empty. These synthetic dylibs won't have synthetic dylibs
  // themselves.
  SmallVector<DylibFile *, 2> extraDylibs;

private:
  DylibFile *getSyntheticDylib(StringRef installName, uint32_t currentVersion,
                               uint32_t compatVersion);

  bool handleLDSymbol(StringRef originalName);
  void handleLDPreviousSymbol(StringRef name, StringRef originalName);
  void handleLDInstallNameSymbol(StringRef name, StringRef originalName);
  void handleLDHideSymbol(StringRef name, StringRef originalName);
  void checkAppExtensionSafety(bool dylibIsAppExtensionSafe) const;
  void parseExportedSymbols(uint32_t offset, uint32_t size);
  void loadReexport(StringRef path, DylibFile *umbrella,
                    const llvm::MachO::InterfaceFile *currentTopLevelTapi);
  void recordExport(llvm::CachedHashStringRef name, DylibFile *file,
                    DylibFile *owner, bool isWeakDef, bool isTlv);

  // What scanExports() reads: the export trie of a Mach-O dylib, or the TBD
  // (kept alive by loadDylib()).
  uint32_t trieOffset = 0;
  uint32_t trieSize = 0;
  const llvm::MachO::InterfaceFile *interface = nullptr;
  bool exportsScanned = false;
  // What it found: the $ld$ symbols in the order seen, and the exports in the
  // order they get recorded in. Names built while scanning live in nameSaver.
  struct ScannedExport {
    llvm::CachedHashStringRef name;
    bool isWeakDef;
    bool isTlv;
  };
  std::vector<StringRef> ldSymbols;
  std::vector<ScannedExport> scannedExports;
  llvm::BumpPtrAllocator nameAlloc;
  llvm::StringSaver nameSaver{nameAlloc};

  // An exported symbol, as the constructor found it: the dylib to attribute
  // it to (usually exportingFile, or a synthetic dylib made for a $ld$previous
  // symbol), and whose `symbols` list it goes on.
  struct PendingExport {
    llvm::CachedHashStringRef name;
    DylibFile *file;
    DylibFile *owner;
    // Its slot in owner->symbols.
    uint32_t ownerIndex;
    bool isWeakDef;
    bool isTlv;
  };
  std::vector<PendingExport> pendingExports;

  // The symbols $ld$hide$ symbols hid, and the position in parse order of
  // the file that hid each one first: a hide applies to the exports of the
  // files from that one on, see recordExports().
  llvm::DenseMap<llvm::CachedHashStringRef, uint32_t> hiddenSymbols;
};

// .a file
class ArchiveFile final : public InputFile {
public:
  explicit ArchiveFile(std::unique_ptr<llvm::object::Archive> &&file,
                       bool forceHidden);
  void addLazySymbols();
  // With deferParse, the member is handed to parseLater() rather than parsed
  // right away.
  void fetch(const llvm::object::Archive::Symbol &, bool deferParse = false);
  // LLD normally doesn't use Error for error-handling, but the underlying
  // Archive library does, so this is the cleanest way to wrap it.
  Error fetch(const llvm::object::Archive::Child &, StringRef reason,
              bool deferParse = false);
  const llvm::object::Archive &getArchive() const { return *file; };
  static bool classof(const InputFile *f) { return f->kind() == ArchiveKind; }

private:
  Expected<InputFile *> childToObjectFile(const llvm::object::Archive::Child &c,
                                          bool lazy, bool deferParse = false);
  std::unique_ptr<llvm::object::Archive> file;
  // Keep track of children fetched from the archive by tracking
  // which address offsets have been fetched already.
  llvm::DenseSet<uint64_t> seen;
  llvm::DenseSet<uint64_t> seenLazy;
  // Load all symbols with hidden visibility (-load_hidden).
  bool forceHidden;
};

class BitcodeFile final : public InputFile {
public:
  // See the ObjFile constructor for deferParse.
  explicit BitcodeFile(MemoryBufferRef mb, StringRef archiveName,
                       uint64_t offsetInArchive, bool lazy = false,
                       bool forceHidden = false, bool compatArch = true,
                       bool deferParse = false);
  static bool classof(const InputFile *f) { return f->kind() == BitcodeKind; }
  void parse();
  void parseLazy();

  std::unique_ptr<llvm::lto::InputFile> obj;
  bool forceHidden;
};

extern llvm::SetVector<InputFile *> inputFiles;
extern llvm::DenseMap<llvm::CachedHashStringRef, MemoryBufferRef> cachedReads;
extern llvm::SmallVector<StringRef> unprocessedLCLinkerOptions;

std::optional<MemoryBufferRef> readFile(StringRef path);

// ObjFile::parse() defers most of its relocation parsing, which is per-file
// work that touches no global state, so that it can be done for all input
// files at once on multiple threads. This runs that deferred work; it must be
// called after all input files (including any produced by LTO) are loaded and
// before anything reads InputSection::relocs.
void parseDeferredRelocations();

void extract(InputFile &file, StringRef reason);

// Parses the files extract() has pulled into the link. May pull in more.
void parsePendingExtracts();
// Queues an object file from the command line to be parsed by the next
// parsePendingObjects(), together with the other files queued since the last
// one. The driver calls that before anything else that adds to the symbol
// table, so files still resolve in command-line order.
void parseLater(InputFile &file);
void parsePendingObjects();

// Opens `paths` on a background thread, in this order, ahead of the readFile()
// calls for them. Opening a file is ~10us of kernel time and the kernel
// serializes open() calls, so this is as fast as the files can be opened; but
// the reader runs while the files before them are parsed. readFile() takes
// the results, parsing the files that are already open if it would otherwise
// have to wait, see there. stopReadingAhead() joins the thread; files that
// were opened but never asked for are dropped.
void startReadingAhead(std::vector<StringRef> paths);
void stopReadingAhead();
// The reader threads also parse the archives they open (their symbol tables
// and member headers), and with -ObjC find the members that flag pulls in;
// this returns that for the archive that readFile() returned `mbref` for, if
// it was one of them.
struct ReadAheadArchive {
  std::unique_ptr<llvm::object::Archive> archive;
  // Whether the -ObjC scan was done. If so, objcSymbols are the archive's
  // symbols that name an ObjC class, in symbol table order, and objcMembers
  // its members with an ObjC section, in member order.
  bool scannedForObjC = false;
  std::vector<llvm::object::Archive::Symbol> objcSymbols;
  std::vector<llvm::object::Archive::Child> objcMembers;
};
std::optional<ReadAheadArchive> takeReadAheadArchive(MemoryBufferRef mbref);

// Unmaps the input files. Nothing may look at their contents after this --
// symbol and section names are references into them -- but the files' names
// stay valid. The kernel would unmap them when the process exits, but that is
// several times slower than doing it here.
void releaseInputBuffers();

namespace detail {

template <class CommandType, class... Types>
std::vector<const CommandType *>
findCommands(const void *anyHdr, size_t maxCommands, Types... types) {
  std::vector<const CommandType *> cmds;
  std::initializer_list<uint32_t> typesList{types...};
  const auto *hdr = reinterpret_cast<const llvm::MachO::mach_header *>(anyHdr);
  const uint8_t *p =
      reinterpret_cast<const uint8_t *>(hdr) + target->headerSize;
  for (uint32_t i = 0, n = hdr->ncmds; i < n; ++i) {
    auto *cmd = reinterpret_cast<const CommandType *>(p);
    if (llvm::is_contained(typesList, cmd->cmd)) {
      cmds.push_back(cmd);
      if (cmds.size() == maxCommands)
        return cmds;
    }
    p += cmd->cmdsize;
  }
  return cmds;
}

} // namespace detail

// anyHdr should be a pointer to either mach_header or mach_header_64
template <class CommandType = llvm::MachO::load_command, class... Types>
const CommandType *findCommand(const void *anyHdr, Types... types) {
  std::vector<const CommandType *> cmds =
      detail::findCommands<CommandType>(anyHdr, 1, types...);
  return cmds.size() ? cmds[0] : nullptr;
}

template <class CommandType = llvm::MachO::load_command, class... Types>
std::vector<const CommandType *> findCommands(const void *anyHdr,
                                              Types... types) {
  return detail::findCommands<CommandType>(anyHdr, 0, types...);
}

std::string replaceThinLTOSuffix(StringRef path);
} // namespace macho

std::string toString(const macho::InputFile *file);
std::string toString(const macho::Section &);
} // namespace lld

#endif
