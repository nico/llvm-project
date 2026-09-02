//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_INPUT_FILES_H
#define LLD_ELF_INPUT_FILES_H

#include "Config.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Reproduce.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Threading.h"

namespace llvm {
struct DILineInfo;
class TarWriter;
namespace lto {
class InputFile;
}
} // namespace llvm

namespace lld {
class DWARFCache;

namespace elf {
class InputSection;
class Symbol;

// Returns "<internal>", "foo.a(bar.o)" or "baz.o".
std::string toStr(Ctx &, const InputFile *f);
const ELFSyncStream &operator<<(const ELFSyncStream &, const InputFile *);

// Opens a given file.
std::optional<MemoryBufferRef> readFile(Ctx &, StringRef path,
                                        llvm::file_magic *magic = nullptr);
// The path an input is read from: --chroot and --remap-inputs applied.
StringRef resolveInputPath(Ctx &, StringRef path);

// Add symbols in File to the symbol table.
// Adds the symbols of the files to the symbol table and extracts the lazy
// files they need (SymbolResolution.cpp). Files added meanwhile (dependent
// libraries) are the caller's to pass in next.
void parseFiles(Ctx &, ArrayRef<InputFile *> files, bool ltoObjects = false);
// Whether the file's ELF kind and machine type match the target, and the
// error for one that does not.
bool isCompatible(Ctx &, InputFile *file);
void reportIncompatible(Ctx &, InputFile *file);
// Adds the library named by a .deplibs entry of file to the link.
void addDependentLibrary(Ctx &, StringRef specifier, const InputFile *file);

// The root class of input files.
class InputFile {
public:
  Ctx &ctx;

protected:
  std::unique_ptr<Symbol *[]> symbols;
  size_t numSymbols = 0;
  SmallVector<InputSectionBase *, 0> sections;

public:
  enum Kind : uint8_t {
    ObjKind,
    SharedKind,
    BitcodeKind,
    BinaryKind,
    InternalKind,
  };

  InputFile(Ctx &, Kind k, MemoryBufferRef m);
  virtual ~InputFile();
  Kind kind() const { return fileKind; }

  bool isElf() const {
    Kind k = kind();
    return k == ObjKind || k == SharedKind;
  }
  bool isInternal() const { return kind() == InternalKind; }

  StringRef getName() const { return mb.getBufferIdentifier(); }
  MemoryBufferRef mb;

  // Returns sections. It is a runtime error to call this function
  // on files that don't have the notion of sections.
  ArrayRef<InputSectionBase *> getSections() const {
    assert(fileKind == ObjKind || fileKind == BinaryKind);
    return sections;
  }
  void cacheDecodedCrel(size_t i, InputSectionBase *s) { sections[i] = s; }

  // Returns object file symbols. It is a runtime error to call this
  // function on files of other types.
  ArrayRef<Symbol *> getSymbols() const {
    assert(fileKind == BinaryKind || fileKind == ObjKind ||
           fileKind == BitcodeKind);
    return {symbols.get(), numSymbols};
  }

  MutableArrayRef<Symbol *> getMutableSymbols() {
    assert(fileKind == BinaryKind || fileKind == ObjKind ||
           fileKind == BitcodeKind);
    return {symbols.get(), numSymbols};
  }

  Symbol &getSymbol(uint32_t symbolIndex) const {
    assert(fileKind == ObjKind);
    if (symbolIndex >= numSymbols)
      Fatal(ctx) << this << ": invalid symbol index";
    return *this->symbols[symbolIndex];
  }

  template <typename RelT> Symbol &getRelocTargetSym(const RelT &rel) const {
    uint32_t symIndex = rel.getSymbol(ctx.arg.isMips64EL);
    return getSymbol(symIndex);
  }

  // Get filename to use for linker script processing.
  StringRef getNameForScript() const;

  // Check if a non-common symbol should be extracted to override a common
  // definition.
  bool shouldExtractForCommon(StringRef name) const;

  // Symbol resolution (SymbolResolution.cpp) sees a file as a list of symbol
  // table operations, "events", numbered in the order the file performs them
  // when parsed on its own, bucketed by the shard of the symbol table the
  // name falls in, definitions before references (an object file resolves
  // all its definitions before its references).
  struct SymbolEvents {
    bool prepared = false;
    uint32_t num = 0;
    std::unique_ptr<uint32_t[]> order;
    // 2 * SymbolTable::numShards + 1 offsets into order.
    std::unique_ptr<uint32_t[]> bounds;
    // Per event, where the symbol table keeps the symbol's data (see
    // SymbolTable::Entry::home); filled by symbol resolution.
    std::unique_ptr<uint32_t[]> homes;
    // Per event, what resolution needs to know about it, so that the hot
    // loops do not read the input's symbol table: Ref/Weak/HasAt/
    // NonDefaultVis/Common as below, Other for file kinds whose events are
    // always resolved exactly.
    enum : uint8_t {
      Ref = 1,
      Weak = 2,
      HasAt = 4,
      NonDefaultVis = 8,
      Common = 16,
      Other = 32,
    };
    std::unique_ptr<uint8_t[]> bits;
    ArrayRef<uint32_t> definitions(unsigned shard) const {
      return {order.get() + bounds[2 * shard],
              order.get() + bounds[2 * shard + 1]};
    }
    ArrayRef<uint32_t> references(unsigned shard) const {
      return {order.get() + bounds[2 * shard + 1],
              order.get() + bounds[2 * shard + 2]};
    }
    // Buckets events 0..n-1 by bucket(e) = 2 * shard + isReference; a
    // negative bucket leaves the event out.
    void build(uint32_t n, llvm::function_ref<int(uint32_t)> bucket);
  };
  SymbolEvents symbolEvents;

  // What the symbol table needs to know about a symbol name besides where it
  // is: the hash of its stem (the name without an @@version suffix) and its
  // length. Computed per file in parallel so that resolution, which is
  // serial per name, does not scan or hash any names.
  struct HashedName {
    uint32_t hash;
    uint32_t size : 31;
    uint32_t hasAt : 1;
  };
  static HashedName hashName(StringRef name);
  // The hashed stem of a name whose HashedName is hn.
  static llvm::CachedHashStringRef hashedStem(StringRef name,
                                              const HashedName &hn);

  // .got2 in the current file. This is used by PPC32 -fPIC/-fPIE to compute
  // offsets in PLT call stubs.
  InputSection *ppc32Got2 = nullptr;

  // Index of MIPS GOT built for this file.
  uint32_t mipsGotIndex = -1;

  // groupId is used for --warn-backrefs which is an optional error
  // checking feature. All files within the same --{start,end}-group or
  // --{start,end}-lib get the same group ID. Otherwise, each file gets a new
  // group ID. For more info, see checkDependency() in SymbolTable.cpp.
  uint32_t groupId = 0;

  // If this is an architecture-specific file, the following members
  // have ELF type (i.e. ELF{32,64}{LE,BE}) and target machine type.
  uint16_t emachine = llvm::ELF::EM_NONE;
  const Kind fileKind;
  ELFKind ekind = ELFNoneKind;
  uint8_t osabi = 0;
  uint8_t abiVersion = 0;

  // True if this is a relocatable object file/bitcode file in an ar archive
  // or between --start-lib and --end-lib.
  bool lazy = false;

  // True if this is an argument for --just-symbols. Usually false.
  bool justSymbols = false;

  // On PPC64 we need to keep track of which files contain small code model
  // relocations that access the .toc section. To minimize the chance of a
  // relocation overflow, files that do contain said relocations should have
  // their .toc sections sorted closer to the .got section than files that do
  // not contain any small code model relocations. Thats because the toc-pointer
  // is defined to point at .got + 0x8000 and the instructions used with small
  // code model relocations support immediates in the range [-0x8000, 0x7FFC],
  // making the addressable range relative to the toc pointer
  // [.got, .got + 0xFFFC].
  bool ppc64SmallCodeModelTocRelocs = false;

public:
  // If not empty, this stores the name of the archive containing this file.
  // We use this string for creating error messages.
  SmallString<0> archiveName;
  // Cache for toStr(Ctx &, const InputFile *). Only toStr should use this
  // member.
  mutable SmallString<0> toStringCache;

private:
  // Cache for getNameForScript().
  mutable SmallString<0> nameForScriptCache;
};

class ELFFileBase : public InputFile {
public:
  ELFFileBase(Ctx &ctx, Kind k, ELFKind ekind, MemoryBufferRef m);
  ~ELFFileBase();
  static bool classof(const InputFile *f) { return f->isElf(); }

  std::atomic<bool> hasFoldedSections{false};

  void init();
  template <typename ELFT> llvm::object::ELFFile<ELFT> getObj() const {
    return check(llvm::object::ELFFile<ELFT>::create(mb.getBuffer()));
  }

  StringRef getStringTable() const { return stringTable; }

  ArrayRef<Symbol *> getLocalSymbols() {
    if (numSymbols == 0)
      return {};
    return llvm::ArrayRef(symbols.get() + 1, firstGlobal - 1);
  }
  ArrayRef<Symbol *> getGlobalSymbols() {
    return llvm::ArrayRef(symbols.get() + firstGlobal,
                          numSymbols - firstGlobal);
  }
  MutableArrayRef<Symbol *> getMutableGlobalSymbols() {
    return llvm::MutableArrayRef(symbols.get() + firstGlobal,
                                     numSymbols - firstGlobal);
  }

  template <typename ELFT> typename ELFT::ShdrRange getELFShdrs() const {
    return typename ELFT::ShdrRange(
        reinterpret_cast<const typename ELFT::Shdr *>(elfShdrs), numELFShdrs);
  }
  template <typename ELFT> typename ELFT::SymRange getELFSyms() const {
    return typename ELFT::SymRange(
        reinterpret_cast<const typename ELFT::Sym *>(elfSyms), numSymbols);
  }
  template <typename ELFT> typename ELFT::SymRange getGlobalELFSyms() const {
    return getELFSyms<ELFT>().slice(firstGlobal);
  }

  // Get cached DWARF information.
  DWARFCache *getDwarf();

  // The hashed stem of global symbol i's name (nameOffset is its st_name),
  // and the name itself.
  llvm::CachedHashStringRef getStem(size_t i, uint32_t nameOffset,
                                    StringRef &name) const {
    const HashedName &hn = hashedNames[i - firstGlobal];
    name = StringRef(stringTable.data() + nameOffset, hn.size);
    return hashedStem(name, hn);
  }

protected:
  // Initializes this class's member variables.
  template <typename ELFT> void init(InputFile::Kind k);
  template <typename ELFT> void hashSymbolNames();

  // For each global symbol; see HashedName.
  std::unique_ptr<HashedName[]> hashedNames;

  StringRef stringTable;
  const void *elfShdrs = nullptr;
  const void *elfSyms = nullptr;
  uint32_t numELFShdrs = 0;
  uint32_t firstGlobal = 0;

  // Below are ObjFile specific members.

  // Debugging information to retrieve source file and line for error
  // reporting. Linker may find reasonable number of errors in a
  // single object file, so we cache debugging information in order to
  // parse it only once for each object file we link.
  llvm::once_flag initDwarf;
  std::unique_ptr<DWARFCache> dwarf;

public:
  // Name of source file obtained from STT_FILE, if present.
  StringRef sourceFile;
  uint32_t andFeatures = 0;
  bool hasCommonSyms = false;
  std::optional<AArch64PauthAbiCoreInfo> aarch64PauthAbiCoreInfo;
};

// .o file.
template <class ELFT> class ObjFile : public ELFFileBase {
  LLVM_ELF_IMPORT_TYPES_ELFT(ELFT)

public:
  static bool classof(const InputFile *f) { return f->kind() == ObjKind; }

  llvm::object::ELFFile<ELFT> getObj() const {
    return this->ELFFileBase::getObj<ELFT>();
  }

  ObjFile(Ctx &ctx, ELFKind ekind, MemoryBufferRef m, StringRef archiveName)
      : ELFFileBase(ctx, ObjKind, ekind, m) {
    this->archiveName = archiveName;
  }

  // The section pre-pass, once the symbols are resolved (see
  // SymbolResolution.cpp): scanComdats() collects the COMDAT group
  // signatures (per file, in parallel), chooseComdats() picks the first file
  // in link order for each signature (per shard of the signature hash, in
  // parallel), parse() applies that and records what needs the file order
  // (dependent libraries, ARM attributes), and parseArmAttributes() merges
  // the latter.
  void scanComdats();
  void chooseComdats(unsigned shard);
  void parse(bool ignoreComdats = false);
  void parseArmAttributes();
  SmallVector<StringRef, 0> dependentLibraries;

  // Symbol events: one per global symbol (see InputFile::SymbolEvents).
  void prepareSymbolEvents();
  const Elf_Sym &eventSym(uint32_t e) const {
    return getELFSyms<ELFT>()[firstGlobal + e];
  }
  bool isReferenceEvent(uint32_t e) const {
    return eventSym(e).st_shndx == llvm::ELF::SHN_UNDEF;
  }
  bool eventNameHasAt(uint32_t e) const { return hashedNames[e].hasAt; }
  llvm::CachedHashStringRef eventName(uint32_t e, StringRef &name) const {
    size_t i = firstGlobal + e;
    return getStem(i, getELFSyms<ELFT>()[i].st_name, name);
  }
  // Resolves global symbol firstGlobal + e as sym: as a lazy definition, or
  // as the definition or reference it is.
  void applyEvent(uint32_t e, Symbol *sym, bool lazy);

  StringRef getShtGroupSignature(ArrayRef<Elf_Shdr> sections,
                                 const Elf_Shdr &sec);

  uint32_t getSectionIndex(const Elf_Sym &sym) const;


  // Pointer to this input file's .llvm_addrsig section, if it has one.
  const Elf_Shdr *addrsigSec = nullptr;

  // SHT_LLVM_CALL_GRAPH_PROFILE section index.
  uint32_t cgProfileSectionIndex = 0;

  // MIPS GP0 value defined by this file. This value represents the gp value
  // used to create the relocatable object and required to support
  // R_MIPS_GPREL16 / R_MIPS_GPREL32 relocations.
  uint32_t mipsGp0 = 0;

  // True if the file defines functions compiled with
  // -fsplit-stack. Usually false.
  bool splitStack = false;

  // True if the file defines functions compiled with -fsplit-stack,
  // but had one or more functions with the no_split_stack attribute.
  bool someNoSplitStack = false;

  void initDwarf();

  void initSectionsAndLocalSyms(bool ignoreComdats);
  void postParse();
  void importCmseSymbols();

private:
  void initializeSections(bool ignoreComdats,
                          const llvm::object::ELFFile<ELFT> &obj);
  void resolveDefined(size_t i);
  void resolveUndefined(size_t i);
  void initializeJustSymbols();

  InputSectionBase *getRelocTarget(uint32_t idx, uint32_t info);
  InputSectionBase *createInputSection(uint32_t idx, const Elf_Shdr &sec,
                                       StringRef name);

  bool shouldMerge(const Elf_Shdr &sec, StringRef name);

  // Each ELF symbol contains a section index which the symbol belongs to.
  // However, because the number of bits dedicated for that is limited, a
  // symbol can directly point to a section only when the section index is
  // equal to or smaller than 65280.
  //
  // If an object file contains more than 65280 sections, the file must
  // contain .symtab_shndx section. The section contains an array of
  // 32-bit integers whose size is the same as the number of symbols.
  // Nth symbol's section index is in the Nth entry of .symtab_shndx.
  //
  // The following variable contains the contents of .symtab_shndx.
  // If the section does not exist (which is common), the array is empty.
  ArrayRef<Elf_Word> shndxTable;

  // Section indices of kept SHT_GROUP sections, recorded by parse() in
  // ascending order, to be used by the parallel initializeSections().
  SmallVector<uint32_t, 0> keptGroups;

  // The COMDAT groups (section index and signature), ordered by the shard
  // of the signature hash with comdatBounds delimiting the shards, and
  // whether each (by section index) is kept: a byte rather than a bit, as
  // the shards are chosen concurrently.
  SmallVector<std::pair<uint32_t, llvm::CachedHashStringRef>, 0> comdats;
  SmallVector<uint32_t, 0> comdatBounds;
  SmallVector<uint8_t, 0> keptComdat;
  uint32_t armAttributesSec = 0;
};

class BitcodeFile : public InputFile {
public:
  BitcodeFile(Ctx &, MemoryBufferRef m, StringRef archiveName,
              uint64_t offsetInArchive, bool lazy);
  static bool classof(const InputFile *f) { return f->kind() == BitcodeKind; }
  // COMDAT selection (like ObjFile's) and dependent libraries, once the
  // symbols are resolved.
  void scanComdats();
  void chooseComdats(unsigned shard);
  void parse();
  void postParse();

  // Symbol events: one per symbol (see InputFile::SymbolEvents).
  void prepareSymbolEvents();
  bool isReferenceEvent(uint32_t e) const;
  llvm::CachedHashStringRef eventName(uint32_t e, StringRef &name) const;
  void applyEvent(uint32_t e, Symbol *sym, bool lazy);

  std::unique_ptr<llvm::lto::InputFile> obj;
  std::vector<bool> keptComdats;

private:
  SmallVector<HashedName, 0> hashedNames;
  SmallVector<std::pair<uint32_t, llvm::CachedHashStringRef>, 0> comdats;
  SmallVector<uint32_t, 0> comdatBounds;
  // Per comdat, chosen concurrently per shard; keptComdats is set from it.
  SmallVector<uint8_t, 0> chosenComdats;
  bool comdatsScanned = false;
};

// .so file.
class SharedFile : public ELFFileBase {
public:
  SharedFile(Ctx &, MemoryBufferRef m, StringRef defaultSoName);

  // This is actually a vector of Elf_Verdef pointers.
  SmallVector<const void *, 0> verdefs;

  // Parallel to verdefs. If a version definition is referenced by a relocatable
  // file, the entry records the assigned Vernaux index in the output file and
  // whether all references are weak.
  struct VerneedInfo {
    uint16_t id = 0;
    // True if all references to this version are weak. Used to set
    // VER_FLG_WEAK.
    bool weak = true;
  };
  SmallVector<VerneedInfo, 0> verneedInfo;

  SmallVector<StringRef, 0> dtNeeded;
  StringRef soName;

  static bool classof(const InputFile *f) { return f->kind() == SharedKind; }

  // Symbol events: two per global dynamic symbol, the symbol and its
  // versioned name (see InputFile::SymbolEvents).
  template <typename ELFT> void prepareSymbolEvents();
  // DSOs are uniquified by soname; returns false if this one is a repeat and
  // adds no symbols.
  bool registerSoName();
  llvm::CachedHashStringRef eventName(uint32_t e, StringRef &name) const {
    name = events[e].name;
    return hashedStem(name, events[e].hashedName);
  }
  bool isReferenceEvent(uint32_t e) const {
    return events[e].kind == Event::Undefined;
  }
  bool isWeakReference(uint32_t e) const { return events[e].weak; }
  bool eventNameHasAt(uint32_t e) const { return events[e].hashedName.hasAt; }
  template <typename ELFT> void applyEvent(uint32_t e, Symbol *sym);
  void finishSymbolEvents();

  // Used for --as-needed
  std::atomic<bool> isNeeded;

  // Non-weak undefined symbols which are not yet resolved when the SO is
  // parsed. Only filled for `--no-allow-shlib-undefined`.
  SmallVector<Symbol *, 0> requiredSymbols;

private:
  struct Event {
    enum Kind : uint8_t { Invalid, Undefined, Shared } kind = Invalid;
    bool weak = false;
    uint16_t versionId = 0;
    uint32_t alignment = 0;
    StringRef name;
    HashedName hashedName;
    Symbol *sym = nullptr;
  };
  SmallVector<Event, 0> events;
  // Storage for the versioned names.
  llvm::BumpPtrAllocator nameAlloc;
  llvm::StringSaver nameSaver{nameAlloc};

  template <typename ELFT>
  std::vector<uint32_t> parseVerneed(const llvm::object::ELFFile<ELFT> &obj,
                                     const typename ELFT::Shdr *sec);
  template <typename ELFT>
  void parseGnuAndFeatures(const llvm::object::ELFFile<ELFT> &obj);
};

class BinaryFile : public InputFile {
public:
  explicit BinaryFile(Ctx &ctx, MemoryBufferRef m)
      : InputFile(ctx, BinaryKind, m) {}
  static bool classof(const InputFile *f) { return f->kind() == BinaryKind; }

  // Symbol events: the _binary_<file>_{start,end,size} definitions.
  void prepareSymbolEvents();
  llvm::CachedHashStringRef eventName(uint32_t e, StringRef &name) const {
    name = names[e];
    return hashedStem(name, hashedNames[e]);
  }
  void applyEvent(uint32_t e, Symbol *sym);

private:
  InputSection *section = nullptr;
  StringRef names[3];
  HashedName hashedNames[3];
};

InputFile *createInternalFile(Ctx &, StringRef name);
std::unique_ptr<ELFFileBase> createObjFile(Ctx &, MemoryBufferRef mb,
                                           StringRef archiveName = "",
                                           bool lazy = false);

std::string replaceThinLTOSuffix(Ctx &, StringRef path);

} // namespace elf
} // namespace lld

#endif
