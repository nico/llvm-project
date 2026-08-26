//===- TextStub.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the text stub file reader/writer.
//
//===----------------------------------------------------------------------===//

#include "TextAPIContext.h"
#include "TextStubCommon.h"
#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TextAPI/Architecture.h"
#include "llvm/TextAPI/ArchitectureSet.h"
#include "llvm/TextAPI/InterfaceFile.h"
#include "llvm/TextAPI/PackedVersion.h"
#include "llvm/TextAPI/TextAPIReader.h"
#include "llvm/TextAPI/TextAPIWriter.h"
#include <deque>
#include <set>

// clang-format off
/*

 YAML Format specification.

 The TBD v1 format only support two level address libraries and is per
 definition application extension safe.

---                              # the tag !tapi-tbd-v1 is optional and
                                 # shouldn't be emitted to support older linker.
archs: [ armv7, armv7s, arm64 ]  # the list of architecture slices that are
                                 # supported by this file.
platform: ios                    # Specifies the platform (macosx, ios, etc)
install-name: /u/l/libfoo.dylib  #
current-version: 1.2.3           # Optional: defaults to 1.0
compatibility-version: 1.0       # Optional: defaults to 1.0
swift-version: 0                 # Optional: defaults to 0
objc-constraint: none            # Optional: defaults to none
exports:                         # List of export sections
...

Each export section is defined as following:

 - archs: [ arm64 ]                   # the list of architecture slices
   allowed-clients: [ client ]        # Optional: List of clients
   re-exports: [ ]                    # Optional: List of re-exports
   symbols: [ _sym ]                  # Optional: List of symbols
   objc-classes: []                   # Optional: List of Objective-C classes
   objc-ivars: []                     # Optional: List of Objective C Instance
                                      #           Variables
   weak-def-symbols: []               # Optional: List of weak defined symbols
   thread-local-symbols: []           # Optional: List of thread local symbols
*/

/*

 YAML Format specification.

--- !tapi-tbd-v2
archs: [ armv7, armv7s, arm64 ]  # the list of architecture slices that are
                                 # supported by this file.
uuids: [ armv7:... ]             # Optional: List of architecture and UUID pairs.
platform: ios                    # Specifies the platform (macosx, ios, etc)
flags: []                        # Optional:
install-name: /u/l/libfoo.dylib  #
current-version: 1.2.3           # Optional: defaults to 1.0
compatibility-version: 1.0       # Optional: defaults to 1.0
swift-version: 0                 # Optional: defaults to 0
objc-constraint: retain_release  # Optional: defaults to retain_release
parent-umbrella:                 # Optional:
exports:                         # List of export sections
...
undefineds:                      # List of undefineds sections
...

Each export section is defined as following:

- archs: [ arm64 ]                   # the list of architecture slices
  allowed-clients: [ client ]        # Optional: List of clients
  re-exports: [ ]                    # Optional: List of re-exports
  symbols: [ _sym ]                  # Optional: List of symbols
  objc-classes: []                   # Optional: List of Objective-C classes
  objc-ivars: []                     # Optional: List of Objective C Instance
                                     #           Variables
  weak-def-symbols: []               # Optional: List of weak defined symbols
  thread-local-symbols: []           # Optional: List of thread local symbols

Each undefineds section is defined as following:
- archs: [ arm64 ]     # the list of architecture slices
  symbols: [ _sym ]    # Optional: List of symbols
  objc-classes: []     # Optional: List of Objective-C classes
  objc-ivars: []       # Optional: List of Objective C Instance Variables
  weak-ref-symbols: [] # Optional: List of weak defined symbols
*/

/*

 YAML Format specification.

--- !tapi-tbd-v3
archs: [ armv7, armv7s, arm64 ]  # the list of architecture slices that are
                                 # supported by this file.
uuids: [ armv7:... ]             # Optional: List of architecture and UUID pairs.
platform: ios                    # Specifies the platform (macosx, ios, etc)
flags: []                        # Optional:
install-name: /u/l/libfoo.dylib  #
current-version: 1.2.3           # Optional: defaults to 1.0
compatibility-version: 1.0       # Optional: defaults to 1.0
swift-abi-version: 0             # Optional: defaults to 0
objc-constraint: retain_release  # Optional: defaults to retain_release
parent-umbrella:                 # Optional:
exports:                         # List of export sections
...
undefineds:                      # List of undefineds sections
...

Each export section is defined as following:

- archs: [ arm64 ]                   # the list of architecture slices
  allowed-clients: [ client ]        # Optional: List of clients
  re-exports: [ ]                    # Optional: List of re-exports
  symbols: [ _sym ]                  # Optional: List of symbols
  objc-classes: []                   # Optional: List of Objective-C classes
  objc-eh-types: []                  # Optional: List of Objective-C classes
                                     #           with EH
  objc-ivars: []                     # Optional: List of Objective C Instance
                                     #           Variables
  weak-def-symbols: []               # Optional: List of weak defined symbols
  thread-local-symbols: []           # Optional: List of thread local symbols

Each undefineds section is defined as following:
- archs: [ arm64 ]     # the list of architecture slices
  symbols: [ _sym ]    # Optional: List of symbols
  objc-classes: []     # Optional: List of Objective-C classes
  objc-eh-types: []                  # Optional: List of Objective-C classes
                                     #           with EH
  objc-ivars: []       # Optional: List of Objective C Instance Variables
  weak-ref-symbols: [] # Optional: List of weak defined symbols
*/

/*

 YAML Format specification.

--- !tapi-tbd
tbd-version: 4                              # The tbd version for format
targets: [ armv7-ios, x86_64-maccatalyst ]  # The list of applicable tapi supported target triples
uuids:                                      # Optional: List of target and UUID pairs.
  - target: armv7-ios
    value: ...
  - target: x86_64-maccatalyst
    value: ...
flags: []                        # Optional:
install-name: /u/l/libfoo.dylib  #
current-version: 1.2.3           # Optional: defaults to 1.0
compatibility-version: 1.0       # Optional: defaults to 1.0
swift-abi-version: 0             # Optional: defaults to 0
parent-umbrella:                 # Optional:
allowable-clients:
  - targets: [ armv7-ios ]       # Optional:
    clients: [ clientA ]
exports:                         # List of export sections
...
re-exports:                      # List of reexport sections
...
undefineds:                      # List of undefineds sections
...

Each export and reexport  section is defined as following:

- targets: [ arm64-macos ]                        # The list of target triples associated with symbols
  symbols: [ _symA ]                              # Optional: List of symbols
  objc-classes: []                                # Optional: List of Objective-C classes
  objc-eh-types: []                               # Optional: List of Objective-C classes
                                                  #           with EH
  objc-ivars: []                                  # Optional: List of Objective C Instance
                                                  #           Variables
  weak-symbols: []                                # Optional: List of weak defined symbols
  thread-local-symbols: []                        # Optional: List of thread local symbols
- targets: [ arm64-macos, x86_64-maccatalyst ]    # Optional: Targets for applicable additional symbols
  symbols: [ _symB ]                              # Optional: List of symbols

Each undefineds section is defined as following:
- targets: [ arm64-macos ]    # The list of target triples associated with symbols
  symbols: [ _symC ]          # Optional: List of symbols
  objc-classes: []            # Optional: List of Objective-C classes
  objc-eh-types: []           # Optional: List of Objective-C classes
                              #           with EH
  objc-ivars: []              # Optional: List of Objective C Instance Variables
  weak-symbols: []            # Optional: List of weak defined symbols
*/
// clang-format on

using namespace llvm;
using namespace llvm::yaml;
using namespace llvm::MachO;

namespace {
struct ExportSection {
  std::vector<Architecture> Architectures;
  std::vector<FlowStringRef> AllowableClients;
  std::vector<FlowStringRef> ReexportedLibraries;
  std::vector<FlowStringRef> Symbols;
  std::vector<FlowStringRef> Classes;
  std::vector<FlowStringRef> ClassEHs;
  std::vector<FlowStringRef> IVars;
  std::vector<FlowStringRef> WeakDefSymbols;
  std::vector<FlowStringRef> TLVSymbols;
};

struct UndefinedSection {
  std::vector<Architecture> Architectures;
  std::vector<FlowStringRef> Symbols;
  std::vector<FlowStringRef> Classes;
  std::vector<FlowStringRef> ClassEHs;
  std::vector<FlowStringRef> IVars;
  std::vector<FlowStringRef> WeakRefSymbols;
};

// Sections for direct target mapping in TBDv4
struct SymbolSection {
  TargetList Targets;
  std::vector<FlowStringRef> Symbols;
  std::vector<FlowStringRef> Classes;
  std::vector<FlowStringRef> ClassEHs;
  std::vector<FlowStringRef> Ivars;
  std::vector<FlowStringRef> WeakSymbols;
  std::vector<FlowStringRef> TlvSymbols;
};

struct MetadataSection {
  enum Option { Clients, Libraries };
  std::vector<Target> Targets;
  std::vector<FlowStringRef> Values;
};

struct UmbrellaSection {
  std::vector<Target> Targets;
  std::string Umbrella;
};

// UUID's for TBDv4 are mapped to target not arch
struct UUIDv4 {
  Target TargetID;
  std::string Value;

  UUIDv4() = default;
  UUIDv4(const Target &TargetID, const std::string &Value)
      : TargetID(TargetID), Value(Value) {}
};
} // end anonymous namespace.

LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(Architecture)
LLVM_YAML_IS_SEQUENCE_VECTOR(ExportSection)
LLVM_YAML_IS_SEQUENCE_VECTOR(UndefinedSection)
// Specific to TBDv4
LLVM_YAML_IS_SEQUENCE_VECTOR(SymbolSection)
LLVM_YAML_IS_SEQUENCE_VECTOR(MetadataSection)
LLVM_YAML_IS_SEQUENCE_VECTOR(UmbrellaSection)
LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(Target)
LLVM_YAML_IS_SEQUENCE_VECTOR(UUIDv4)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<ExportSection> {
  static void mapping(IO &IO, ExportSection &Section) {
    const auto *Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
    assert((!Ctx || Ctx->FileKind != FileType::Invalid) &&
           "File type is not set in YAML context");

    IO.mapRequired("archs", Section.Architectures);
    if (Ctx->FileKind == FileType::TBD_V1)
      IO.mapOptional("allowed-clients", Section.AllowableClients);
    else
      IO.mapOptional("allowable-clients", Section.AllowableClients);
    IO.mapOptional("re-exports", Section.ReexportedLibraries);
    IO.mapOptional("symbols", Section.Symbols);
    IO.mapOptional("objc-classes", Section.Classes);
    if (Ctx->FileKind == FileType::TBD_V3)
      IO.mapOptional("objc-eh-types", Section.ClassEHs);
    IO.mapOptional("objc-ivars", Section.IVars);
    IO.mapOptional("weak-def-symbols", Section.WeakDefSymbols);
    IO.mapOptional("thread-local-symbols", Section.TLVSymbols);
  }
};

template <> struct MappingTraits<UndefinedSection> {
  static void mapping(IO &IO, UndefinedSection &Section) {
    const auto *Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
    assert((!Ctx || Ctx->FileKind != FileType::Invalid) &&
           "File type is not set in YAML context");

    IO.mapRequired("archs", Section.Architectures);
    IO.mapOptional("symbols", Section.Symbols);
    IO.mapOptional("objc-classes", Section.Classes);
    if (Ctx->FileKind == FileType::TBD_V3)
      IO.mapOptional("objc-eh-types", Section.ClassEHs);
    IO.mapOptional("objc-ivars", Section.IVars);
    IO.mapOptional("weak-ref-symbols", Section.WeakRefSymbols);
  }
};

template <> struct MappingTraits<SymbolSection> {
  static void mapping(IO &IO, SymbolSection &Section) {
    IO.mapRequired("targets", Section.Targets);
    // With SkipUnknownTriples, ScalarTraits of Target accepts unknown
    // arch/platform scalars without erroring, leaving invalid Targets in the
    // vector. Drop them so downstream code only sees valid Targets.
    if (!IO.outputting())
      llvm::erase_if(Section.Targets,
                     [](const Target &T) { return !T.isValid(); });
    IO.mapOptional("symbols", Section.Symbols);
    IO.mapOptional("objc-classes", Section.Classes);
    IO.mapOptional("objc-eh-types", Section.ClassEHs);
    IO.mapOptional("objc-ivars", Section.Ivars);
    IO.mapOptional("weak-symbols", Section.WeakSymbols);
    IO.mapOptional("thread-local-symbols", Section.TlvSymbols);
  }
};

template <> struct MappingTraits<UmbrellaSection> {
  static void mapping(IO &IO, UmbrellaSection &Section) {
    IO.mapRequired("targets", Section.Targets);
    if (!IO.outputting())
      llvm::erase_if(Section.Targets,
                     [](const Target &T) { return !T.isValid(); });
    IO.mapRequired("umbrella", Section.Umbrella);
  }
};

template <> struct MappingTraits<UUIDv4> {
  static void mapping(IO &IO, UUIDv4 &UUID) {
    IO.mapRequired("target", UUID.TargetID);
    IO.mapRequired("value", UUID.Value);
  }
};

template <>
struct MappingContextTraits<MetadataSection, MetadataSection::Option> {
  static void mapping(IO &IO, MetadataSection &Section,
                      MetadataSection::Option &OptionKind) {
    IO.mapRequired("targets", Section.Targets);
    if (!IO.outputting())
      llvm::erase_if(Section.Targets,
                     [](const Target &T) { return !T.isValid(); });
    switch (OptionKind) {
    case MetadataSection::Option::Clients:
      IO.mapRequired("clients", Section.Values);
      return;
    case MetadataSection::Option::Libraries:
      IO.mapRequired("libraries", Section.Values);
      return;
    }
    llvm_unreachable("unexpected option for metadata");
  }
};

template <> struct ScalarBitSetTraits<TBDFlags> {
  static void bitset(IO &IO, TBDFlags &Flags) {
    IO.bitSetCase(Flags, "flat_namespace", TBDFlags::FlatNamespace);
    IO.bitSetCase(Flags, "not_app_extension_safe",
                  TBDFlags::NotApplicationExtensionSafe);
    IO.bitSetCase(Flags, "installapi", TBDFlags::InstallAPI);
    IO.bitSetCase(Flags, "not_for_dyld_shared_cache",
                  TBDFlags::OSLibNotForSharedCache);
  }
};

template <> struct ScalarTraits<Target> {
  static void output(const Target &Value, void *, raw_ostream &OS) {
    OS << Value.Arch << "-";
    switch (Value.Platform) {
#define PLATFORM(platform, id, name, build_name, target, tapi_target,          \
                 marketing)                                                    \
  case PLATFORM_##platform:                                                    \
    OS << #tapi_target;                                                        \
    break;
#include "llvm/BinaryFormat/MachO.def"
    }
  }

  static StringRef input(StringRef Scalar, void *Ctx, Target &Value) {
    auto Result = Target::create(Scalar);
    if (!Result) {
      consumeError(Result.takeError());
      return "unparsable target";
    }

    Value = *Result;

    const bool SkipUnknownTriples =
        reinterpret_cast<TextAPIContext *>(Ctx)->SkipUnknownTriples;
    if (!Value.isValid() && !SkipUnknownTriples)
      return "unknown target";

    return {};
  }

  static QuotingType mustQuote(StringRef) { return QuotingType::None; }
};

template <> struct MappingTraits<const InterfaceFile *> {
  struct NormalizedTBD {
    explicit NormalizedTBD(IO &IO) {}
    NormalizedTBD(IO &IO, const InterfaceFile *&File) {
      Architectures = File->getArchitectures();
      Platforms = File->getPlatforms();
      InstallName = File->getInstallName();
      CurrentVersion = PackedVersion(File->getCurrentVersion());
      CompatibilityVersion = PackedVersion(File->getCompatibilityVersion());
      SwiftABIVersion = File->getSwiftABIVersion();
      ObjCConstraint = File->getObjCConstraint();

      Flags = TBDFlags::None;
      if (!File->isApplicationExtensionSafe())
        Flags |= TBDFlags::NotApplicationExtensionSafe;

      if (!File->isTwoLevelNamespace())
        Flags |= TBDFlags::FlatNamespace;

      if (!File->umbrellas().empty())
        ParentUmbrella = File->umbrellas().begin()->second;

      std::set<ArchitectureSet> ArchSet;
      for (const auto &Library : File->allowableClients())
        ArchSet.insert(Library.getArchitectures());

      for (const auto &Library : File->reexportedLibraries())
        ArchSet.insert(Library.getArchitectures());

      std::map<const Symbol *, ArchitectureSet> SymbolToArchSet;
      for (const auto *Symbol : File->symbols()) {
        auto Architectures = Symbol->getArchitectures();
        SymbolToArchSet[Symbol] = Architectures;
        ArchSet.insert(Architectures);
      }

      for (auto Architectures : ArchSet) {
        ExportSection Section;
        Section.Architectures = Architectures;

        for (const auto &Library : File->allowableClients())
          if (Library.getArchitectures() == Architectures)
            Section.AllowableClients.emplace_back(Library.getInstallName());

        for (const auto &Library : File->reexportedLibraries())
          if (Library.getArchitectures() == Architectures)
            Section.ReexportedLibraries.emplace_back(Library.getInstallName());

        for (const auto &SymArch : SymbolToArchSet) {
          if (SymArch.second != Architectures)
            continue;

          const auto *Symbol = SymArch.first;
          switch (Symbol->getKind()) {
          case EncodeKind::GlobalSymbol:
            if (Symbol->isWeakDefined())
              Section.WeakDefSymbols.emplace_back(Symbol->getName());
            else if (Symbol->isThreadLocalValue())
              Section.TLVSymbols.emplace_back(Symbol->getName());
            else
              Section.Symbols.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCClass:
            if (File->getFileType() != FileType::TBD_V3)
              Section.Classes.emplace_back(
                  copyString("_" + Symbol->getName().str()));
            else
              Section.Classes.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCClassEHType:
            if (File->getFileType() != FileType::TBD_V3)
              Section.Symbols.emplace_back(
                  copyString("_OBJC_EHTYPE_$_" + Symbol->getName().str()));
            else
              Section.ClassEHs.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCInstanceVariable:
            if (File->getFileType() != FileType::TBD_V3)
              Section.IVars.emplace_back(
                  copyString("_" + Symbol->getName().str()));
            else
              Section.IVars.emplace_back(Symbol->getName());
            break;
          }
        }
        llvm::sort(Section.Symbols);
        llvm::sort(Section.Classes);
        llvm::sort(Section.ClassEHs);
        llvm::sort(Section.IVars);
        llvm::sort(Section.WeakDefSymbols);
        llvm::sort(Section.TLVSymbols);
        Exports.emplace_back(std::move(Section));
      }

      ArchSet.clear();
      SymbolToArchSet.clear();

      for (const auto *Symbol : File->undefineds()) {
        auto Architectures = Symbol->getArchitectures();
        SymbolToArchSet[Symbol] = Architectures;
        ArchSet.insert(Architectures);
      }

      for (auto Architectures : ArchSet) {
        UndefinedSection Section;
        Section.Architectures = Architectures;

        for (const auto &SymArch : SymbolToArchSet) {
          if (SymArch.second != Architectures)
            continue;

          const auto *Symbol = SymArch.first;
          switch (Symbol->getKind()) {
          case EncodeKind::GlobalSymbol:
            if (Symbol->isWeakReferenced())
              Section.WeakRefSymbols.emplace_back(Symbol->getName());
            else
              Section.Symbols.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCClass:
            if (File->getFileType() != FileType::TBD_V3)
              Section.Classes.emplace_back(
                  copyString("_" + Symbol->getName().str()));
            else
              Section.Classes.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCClassEHType:
            if (File->getFileType() != FileType::TBD_V3)
              Section.Symbols.emplace_back(
                  copyString("_OBJC_EHTYPE_$_" + Symbol->getName().str()));
            else
              Section.ClassEHs.emplace_back(Symbol->getName());
            break;
          case EncodeKind::ObjectiveCInstanceVariable:
            if (File->getFileType() != FileType::TBD_V3)
              Section.IVars.emplace_back(
                  copyString("_" + Symbol->getName().str()));
            else
              Section.IVars.emplace_back(Symbol->getName());
            break;
          }
        }
        llvm::sort(Section.Symbols);
        llvm::sort(Section.Classes);
        llvm::sort(Section.ClassEHs);
        llvm::sort(Section.IVars);
        llvm::sort(Section.WeakRefSymbols);
        Undefineds.emplace_back(std::move(Section));
      }
    }

    // TBD v1 - TBD v3 files only support one platform and several
    // architectures. It is possible to have more than one platform for TBD v3
    // files, but the architectures don't apply to all
    // platforms, specifically to filter out the i386 slice from
    // platform macCatalyst.
    TargetList synthesizeTargets(ArchitectureSet Architectures,
                                 const PlatformSet &Platforms) {
      TargetList Targets;

      for (auto Platform : Platforms) {
        Platform = mapToPlatformType(Platform, Architectures.hasX86());

        for (const auto &&Architecture : Architectures) {
          if ((Architecture == AK_i386) && (Platform == PLATFORM_MACCATALYST))
            continue;

          Target T(Architecture, Platform);
          if (!T.isValid())
            continue;
          Targets.push_back(T);
        }
      }
      return Targets;
    }

    const InterfaceFile *denormalize(IO &IO) {
      auto Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
      assert(Ctx);

      auto *File = new InterfaceFile;
      File->setPath(Ctx->Path);
      File->setFileType(Ctx->FileKind);
      File->addTargets(synthesizeTargets(Architectures, Platforms));
      File->setInstallName(InstallName);
      File->setCurrentVersion(CurrentVersion);
      File->setCompatibilityVersion(CompatibilityVersion);
      File->setSwiftABIVersion(SwiftABIVersion);
      File->setObjCConstraint(ObjCConstraint);
      for (const auto &Target : File->targets())
        File->addParentUmbrella(Target, ParentUmbrella);

      if (Ctx->FileKind == FileType::TBD_V1) {
        File->setTwoLevelNamespace();
        File->setApplicationExtensionSafe();
      } else {
        File->setTwoLevelNamespace(!(Flags & TBDFlags::FlatNamespace));
        File->setApplicationExtensionSafe(
            !(Flags & TBDFlags::NotApplicationExtensionSafe));
      }

      // For older file formats, the segment where the symbol
      // comes from is unknown, treat all symbols as Data
      // in these cases.
      const auto Flags = SymbolFlags::Data;

      for (const auto &Section : Exports) {
        const auto Targets =
            synthesizeTargets(Section.Architectures, Platforms);
        if (Targets.empty())
          continue;

        for (const auto &Lib : Section.AllowableClients)
          for (const auto &Target : Targets)
            File->addAllowableClient(Lib, Target);

        for (const auto &Lib : Section.ReexportedLibraries)
          for (const auto &Target : Targets)
            File->addReexportedLibrary(Lib, Target);

        for (const auto &Symbol : Section.Symbols) {
          if (Ctx->FileKind != FileType::TBD_V3 &&
              Symbol.value.starts_with(ObjC2EHTypePrefix))
            File->addSymbol(EncodeKind::ObjectiveCClassEHType,
                            Symbol.value.drop_front(15), Targets, Flags);
          else
            File->addSymbol(EncodeKind::GlobalSymbol, Symbol, Targets, Flags);
        }
        for (auto &Symbol : Section.Classes) {
          auto Name = Symbol.value;
          if (Ctx->FileKind != FileType::TBD_V3)
            Name = Name.drop_front();
          File->addSymbol(EncodeKind::ObjectiveCClass, Name, Targets, Flags);
        }
        for (auto &Symbol : Section.ClassEHs)
          File->addSymbol(EncodeKind::ObjectiveCClassEHType, Symbol, Targets,
                          Flags);
        for (auto &Symbol : Section.IVars) {
          auto Name = Symbol.value;
          if (Ctx->FileKind != FileType::TBD_V3)
            Name = Name.drop_front();
          File->addSymbol(EncodeKind::ObjectiveCInstanceVariable, Name, Targets,
                          Flags);
        }
        for (auto &Symbol : Section.WeakDefSymbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Symbol, Targets,
                          SymbolFlags::WeakDefined | Flags);
        for (auto &Symbol : Section.TLVSymbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Symbol, Targets,
                          SymbolFlags::ThreadLocalValue | Flags);
      }

      for (const auto &Section : Undefineds) {
        const auto Targets =
            synthesizeTargets(Section.Architectures, Platforms);
        if (Targets.empty())
          continue;
        for (auto &Symbol : Section.Symbols) {
          if (Ctx->FileKind != FileType::TBD_V3 &&
              Symbol.value.starts_with(ObjC2EHTypePrefix))
            File->addSymbol(EncodeKind::ObjectiveCClassEHType,
                            Symbol.value.drop_front(15), Targets,
                            SymbolFlags::Undefined | Flags);
          else
            File->addSymbol(EncodeKind::GlobalSymbol, Symbol, Targets,
                            SymbolFlags::Undefined | Flags);
        }
        for (auto &Symbol : Section.Classes) {
          auto Name = Symbol.value;
          if (Ctx->FileKind != FileType::TBD_V3)
            Name = Name.drop_front();
          File->addSymbol(EncodeKind::ObjectiveCClass, Name, Targets,
                          SymbolFlags::Undefined | Flags);
        }
        for (auto &Symbol : Section.ClassEHs)
          File->addSymbol(EncodeKind::ObjectiveCClassEHType, Symbol, Targets,
                          SymbolFlags::Undefined | Flags);
        for (auto &Symbol : Section.IVars) {
          auto Name = Symbol.value;
          if (Ctx->FileKind != FileType::TBD_V3)
            Name = Name.drop_front();
          File->addSymbol(EncodeKind::ObjectiveCInstanceVariable, Name, Targets,
                          SymbolFlags::Undefined | Flags);
        }
        for (auto &Symbol : Section.WeakRefSymbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Symbol, Targets,
                          SymbolFlags::Undefined | SymbolFlags::WeakReferenced |
                              Flags);
      }

      return File;
    }

    llvm::BumpPtrAllocator Allocator;
    StringRef copyString(StringRef String) {
      if (String.empty())
        return {};

      void *Ptr = Allocator.Allocate(String.size(), 1);
      memcpy(Ptr, String.data(), String.size());
      return StringRef(reinterpret_cast<const char *>(Ptr), String.size());
    }

    std::vector<Architecture> Architectures;
    std::vector<UUID> UUIDs;
    PlatformSet Platforms;
    StringRef InstallName;
    PackedVersion CurrentVersion;
    PackedVersion CompatibilityVersion;
    SwiftVersion SwiftABIVersion{0};
    ObjCConstraintType ObjCConstraint{ObjCConstraintType::None};
    TBDFlags Flags{TBDFlags::None};
    StringRef ParentUmbrella;
    std::vector<ExportSection> Exports;
    std::vector<UndefinedSection> Undefineds;
  };

  static void setFileTypeForInput(TextAPIContext *Ctx, IO &IO) {
    if (IO.mapTag("!tapi-tbd", false))
      Ctx->FileKind = FileType::TBD_V4;
    else if (IO.mapTag("!tapi-tbd-v3", false))
      Ctx->FileKind = FileType::TBD_V3;
    else if (IO.mapTag("!tapi-tbd-v2", false))
      Ctx->FileKind = FileType::TBD_V2;
    else if (IO.mapTag("!tapi-tbd-v1", false) ||
             IO.mapTag("tag:yaml.org,2002:map", false))
      Ctx->FileKind = FileType::TBD_V1;
    else {
      Ctx->FileKind = FileType::Invalid;
      return;
    }
  }

  static void mapping(IO &IO, const InterfaceFile *&File) {
    auto *Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
    assert((!Ctx || !IO.outputting() ||
            (Ctx && Ctx->FileKind != FileType::Invalid)) &&
           "File type is not set in YAML context");

    if (!IO.outputting()) {
      setFileTypeForInput(Ctx, IO);
      switch (Ctx->FileKind) {
      default:
        break;
      case FileType::TBD_V4:
        mapKeysToValuesV4(IO, File);
        return;
      case FileType::Invalid:
        IO.setError("unsupported file type");
        return;
      }
    } else {
      // Set file type when writing.
      switch (Ctx->FileKind) {
      default:
        llvm_unreachable("unexpected file type");
      case FileType::TBD_V4:
        mapKeysToValuesV4(IO, File);
        return;
      case FileType::TBD_V3:
        IO.mapTag("!tapi-tbd-v3", true);
        break;
      case FileType::TBD_V2:
        IO.mapTag("!tapi-tbd-v2", true);
        break;
      case FileType::TBD_V1:
        // Don't write the tag into the .tbd file for TBD v1
        break;
      }
    }
    mapKeysToValues(Ctx->FileKind, IO, File);
  }

  using SectionList = std::vector<SymbolSection>;
  struct NormalizedTBD_V4 {
    explicit NormalizedTBD_V4(IO &IO) {}
    NormalizedTBD_V4(IO &IO, const InterfaceFile *&File) {
      auto Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
      assert(Ctx);
      TBDVersion = Ctx->FileKind >> 4;
      for (auto &T : File->targets())
        if (T.isValid())
          Targets.push_back(T);
      InstallName = File->getInstallName();
      CurrentVersion = File->getCurrentVersion();
      CompatibilityVersion = File->getCompatibilityVersion();
      SwiftABIVersion = File->getSwiftABIVersion();

      Flags = TBDFlags::None;
      if (!File->isApplicationExtensionSafe())
        Flags |= TBDFlags::NotApplicationExtensionSafe;

      if (!File->isTwoLevelNamespace())
        Flags |= TBDFlags::FlatNamespace;

      if (File->isOSLibNotForSharedCache())
        Flags |= TBDFlags::OSLibNotForSharedCache;

      {
        std::map<std::string, TargetList> valueToTargetList;
        for (const auto &it : File->umbrellas())
          if (it.first.isValid())
            valueToTargetList[it.second].emplace_back(it.first);

        for (const auto &it : valueToTargetList) {
          UmbrellaSection CurrentSection;
          CurrentSection.Targets.insert(CurrentSection.Targets.begin(),
                                        it.second.begin(), it.second.end());
          CurrentSection.Umbrella = it.first;
          ParentUmbrellas.emplace_back(std::move(CurrentSection));
        }
      }

      assignTargetsToLibrary(File->allowableClients(), AllowableClients);
      assignTargetsToLibrary(File->reexportedLibraries(), ReexportedLibraries);

      auto handleSymbols =
          [](SectionList &CurrentSections,
             InterfaceFile::const_filtered_symbol_range Symbols) {
            std::set<TargetList> TargetSet;
            std::map<const Symbol *, TargetList> SymbolToTargetList;
            for (const auto *Symbol : Symbols) {
              TargetList Targets;
              for (auto &T : Symbol->targets())
                if (T.isValid())
                  Targets.push_back(T);

              SymbolToTargetList[Symbol] = Targets;
              TargetSet.emplace(std::move(Targets));
            }
            for (const auto &TargetIDs : TargetSet) {
              SymbolSection CurrentSection;
              CurrentSection.Targets.insert(CurrentSection.Targets.begin(),
                                            TargetIDs.begin(), TargetIDs.end());

              for (const auto &IT : SymbolToTargetList) {
                if (IT.second != TargetIDs)
                  continue;

                const auto *Symbol = IT.first;
                switch (Symbol->getKind()) {
                case EncodeKind::GlobalSymbol:
                  if (Symbol->isWeakDefined())
                    CurrentSection.WeakSymbols.emplace_back(Symbol->getName());
                  else if (Symbol->isThreadLocalValue())
                    CurrentSection.TlvSymbols.emplace_back(Symbol->getName());
                  else
                    CurrentSection.Symbols.emplace_back(Symbol->getName());
                  break;
                case EncodeKind::ObjectiveCClass:
                  CurrentSection.Classes.emplace_back(Symbol->getName());
                  break;
                case EncodeKind::ObjectiveCClassEHType:
                  CurrentSection.ClassEHs.emplace_back(Symbol->getName());
                  break;
                case EncodeKind::ObjectiveCInstanceVariable:
                  CurrentSection.Ivars.emplace_back(Symbol->getName());
                  break;
                }
              }
              sort(CurrentSection.Symbols);
              sort(CurrentSection.Classes);
              sort(CurrentSection.ClassEHs);
              sort(CurrentSection.Ivars);
              sort(CurrentSection.WeakSymbols);
              sort(CurrentSection.TlvSymbols);
              CurrentSections.emplace_back(std::move(CurrentSection));
            }
          };

      handleSymbols(Exports, File->exports());
      handleSymbols(Reexports, File->reexports());
      handleSymbols(Undefineds, File->undefineds());
    }

    const InterfaceFile *denormalize(IO &IO) {
      auto Ctx = reinterpret_cast<TextAPIContext *>(IO.getContext());
      assert(Ctx);

      auto *File = new InterfaceFile;
      File->setPath(Ctx->Path);
      File->setFileType(Ctx->FileKind);
      File->addTargets(Targets);
      File->setInstallName(InstallName);
      File->setCurrentVersion(CurrentVersion);
      File->setCompatibilityVersion(CompatibilityVersion);
      File->setSwiftABIVersion(SwiftABIVersion);
      for (const auto &CurrentSection : ParentUmbrellas)
        for (const auto &target : CurrentSection.Targets)
          File->addParentUmbrella(target, CurrentSection.Umbrella);
      File->setTwoLevelNamespace(!(Flags & TBDFlags::FlatNamespace));
      File->setApplicationExtensionSafe(
          !(Flags & TBDFlags::NotApplicationExtensionSafe));
      File->setOSLibNotForSharedCache(
          (Flags & TBDFlags::OSLibNotForSharedCache));

      for (const auto &CurrentSection : AllowableClients) {
        for (const auto &lib : CurrentSection.Values)
          for (const auto &Target : CurrentSection.Targets)
            File->addAllowableClient(lib, Target);
      }

      for (const auto &CurrentSection : ReexportedLibraries) {
        for (const auto &Lib : CurrentSection.Values)
          for (const auto &Target : CurrentSection.Targets)
            File->addReexportedLibrary(Lib, Target);
      }

      auto handleSymbols = [File](const SectionList &CurrentSections,
                                  SymbolFlags InputFlag = SymbolFlags::None) {
        // For older file formats, the segment where the symbol
        // comes from is unknown, treat all symbols as Data
        // in these cases.
        const SymbolFlags Flag = InputFlag | SymbolFlags::Data;

        for (const auto &CurrentSection : CurrentSections) {
          if (CurrentSection.Targets.empty())
            continue;

          for (auto &sym : CurrentSection.Symbols)
            File->addSymbol(EncodeKind::GlobalSymbol, sym,
                            CurrentSection.Targets, Flag);

          for (auto &sym : CurrentSection.Classes)
            File->addSymbol(EncodeKind::ObjectiveCClass, sym,
                            CurrentSection.Targets, Flag);

          for (auto &sym : CurrentSection.ClassEHs)
            File->addSymbol(EncodeKind::ObjectiveCClassEHType, sym,
                            CurrentSection.Targets, Flag);

          for (auto &sym : CurrentSection.Ivars)
            File->addSymbol(EncodeKind::ObjectiveCInstanceVariable, sym,
                            CurrentSection.Targets, Flag);

          SymbolFlags SymFlag =
              ((Flag & SymbolFlags::Undefined) == SymbolFlags::Undefined)
                  ? SymbolFlags::WeakReferenced
                  : SymbolFlags::WeakDefined;
          for (auto &sym : CurrentSection.WeakSymbols) {
            File->addSymbol(EncodeKind::GlobalSymbol, sym,
                            CurrentSection.Targets, Flag | SymFlag);
          }

          for (auto &sym : CurrentSection.TlvSymbols)
            File->addSymbol(EncodeKind::GlobalSymbol, sym,
                            CurrentSection.Targets,
                            Flag | SymbolFlags::ThreadLocalValue);
        }
      };

      handleSymbols(Exports);
      handleSymbols(Reexports, SymbolFlags::Rexported);
      handleSymbols(Undefineds, SymbolFlags::Undefined);

      return File;
    }

    unsigned TBDVersion;
    std::vector<UUIDv4> UUIDs;
    TargetList Targets;
    StringRef InstallName;
    PackedVersion CurrentVersion;
    PackedVersion CompatibilityVersion;
    SwiftVersion SwiftABIVersion{0};
    std::vector<MetadataSection> AllowableClients;
    std::vector<MetadataSection> ReexportedLibraries;
    TBDFlags Flags{TBDFlags::None};
    std::vector<UmbrellaSection> ParentUmbrellas;
    SectionList Exports;
    SectionList Reexports;
    SectionList Undefineds;

  private:
    void assignTargetsToLibrary(const std::vector<InterfaceFileRef> &Libraries,
                                std::vector<MetadataSection> &Section) {
      std::set<TargetList> targetSet;
      std::map<const InterfaceFileRef *, TargetList> valueToTargetList;
      for (const auto &library : Libraries) {
        TargetList targets(library.targets());
        valueToTargetList[&library] = targets;
        targetSet.emplace(std::move(targets));
      }

      for (const auto &targets : targetSet) {
        MetadataSection CurrentSection;
        CurrentSection.Targets.insert(CurrentSection.Targets.begin(),
                                      targets.begin(), targets.end());

        for (const auto &it : valueToTargetList) {
          if (it.second != targets)
            continue;

          CurrentSection.Values.emplace_back(it.first->getInstallName());
        }
        llvm::sort(CurrentSection.Values);
        Section.emplace_back(std::move(CurrentSection));
      }
    }
  };

  static void mapKeysToValues(FileType FileKind, IO &IO,
                              const InterfaceFile *&File) {
    MappingNormalization<NormalizedTBD, const InterfaceFile *> Keys(IO, File);
    std::vector<UUID> EmptyUUID;
    IO.mapRequired("archs", Keys->Architectures);
    if (FileKind != FileType::TBD_V1)
      IO.mapOptional("uuids", EmptyUUID);
    IO.mapRequired("platform", Keys->Platforms);
    if (FileKind != FileType::TBD_V1)
      IO.mapOptional("flags", Keys->Flags, TBDFlags::None);
    IO.mapRequired("install-name", Keys->InstallName);
    IO.mapOptional("current-version", Keys->CurrentVersion,
                   PackedVersion(1, 0, 0));
    IO.mapOptional("compatibility-version", Keys->CompatibilityVersion,
                   PackedVersion(1, 0, 0));
    if (FileKind != FileType::TBD_V3)
      IO.mapOptional("swift-version", Keys->SwiftABIVersion, SwiftVersion(0));
    else
      IO.mapOptional("swift-abi-version", Keys->SwiftABIVersion,
                     SwiftVersion(0));
    IO.mapOptional("objc-constraint", Keys->ObjCConstraint,
                   (FileKind == FileType::TBD_V1)
                       ? ObjCConstraintType::None
                       : ObjCConstraintType::Retain_Release);
    if (FileKind != FileType::TBD_V1)
      IO.mapOptional("parent-umbrella", Keys->ParentUmbrella, StringRef());
    IO.mapOptional("exports", Keys->Exports);
    if (FileKind != FileType::TBD_V1)
      IO.mapOptional("undefineds", Keys->Undefineds);
  }

  static void mapKeysToValuesV4(IO &IO, const InterfaceFile *&File) {
    MappingNormalization<NormalizedTBD_V4, const InterfaceFile *> Keys(IO,
                                                                       File);
    std::vector<UUIDv4> EmptyUUID;
    IO.mapTag("!tapi-tbd", true);
    IO.mapRequired("tbd-version", Keys->TBDVersion);
    IO.mapRequired("targets", Keys->Targets);
    if (!IO.outputting())
      llvm::erase_if(Keys->Targets,
                     [](const Target &T) { return !T.isValid(); });
    IO.mapOptional("uuids", EmptyUUID);
    IO.mapOptional("flags", Keys->Flags, TBDFlags::None);
    IO.mapRequired("install-name", Keys->InstallName);
    IO.mapOptional("current-version", Keys->CurrentVersion,
                   PackedVersion(1, 0, 0));
    IO.mapOptional("compatibility-version", Keys->CompatibilityVersion,
                   PackedVersion(1, 0, 0));
    IO.mapOptional("swift-abi-version", Keys->SwiftABIVersion, SwiftVersion(0));
    IO.mapOptional("parent-umbrella", Keys->ParentUmbrellas);
    auto OptionKind = MetadataSection::Option::Clients;
    IO.mapOptionalWithContext("allowable-clients", Keys->AllowableClients,
                              OptionKind);
    OptionKind = MetadataSection::Option::Libraries;
    IO.mapOptionalWithContext("reexported-libraries", Keys->ReexportedLibraries,
                              OptionKind);
    IO.mapOptional("exports", Keys->Exports);
    IO.mapOptional("reexports", Keys->Reexports);
    IO.mapOptional("undefineds", Keys->Undefineds);
  }
};

template <>
struct DocumentListTraits<std::vector<const MachO::InterfaceFile *>> {
  static size_t size(IO &IO, std::vector<const MachO::InterfaceFile *> &Seq) {
    return Seq.size();
  }
  static const InterfaceFile *&
  element(IO &IO, std::vector<const InterfaceFile *> &Seq, size_t Index) {
    if (Index >= Seq.size())
      Seq.resize(Index + 1);
    return Seq[Index];
  }
};

} // end namespace yaml.
} // namespace llvm

static void DiagHandler(const SMDiagnostic &Diag, void *Context) {
  auto *File = static_cast<TextAPIContext *>(Context);
  SmallString<1024> Message;
  raw_svector_ostream S(Message);

  SMDiagnostic NewDiag(*Diag.getSourceMgr(), Diag.getLoc(), File->Path,
                       Diag.getLineNo(), Diag.getColumnNo(), Diag.getKind(),
                       Diag.getMessage(), Diag.getLineContents(),
                       Diag.getRanges(), Diag.getFixIts());

  NewDiag.print(nullptr, S);
  File->ErrorMessage = ("malformed file\n" + Message).str();
}

Expected<FileType> TextAPIReader::canRead(MemoryBufferRef InputBuffer) {
  auto TAPIFile = InputBuffer.getBuffer().trim();
  if (TAPIFile.starts_with("{") && TAPIFile.ends_with("}"))
    return FileType::TBD_V5;

  if (!TAPIFile.ends_with("..."))
    return createStringError(std::errc::not_supported, "unsupported file type");

  if (TAPIFile.starts_with("--- !tapi-tbd"))
    return FileType::TBD_V4;

  if (TAPIFile.starts_with("--- !tapi-tbd-v3"))
    return FileType::TBD_V3;

  if (TAPIFile.starts_with("--- !tapi-tbd-v2"))
    return FileType::TBD_V2;

  if (TAPIFile.starts_with("--- !tapi-tbd-v1") ||
      TAPIFile.starts_with("---\narchs:"))
    return FileType::TBD_V1;

  return createStringError(std::errc::not_supported, "unsupported file type");
}

namespace {

// A reader for the subset of YAML that TBD v4 files are written in (by tapi,
// and by TextAPIWriter): block mappings, block sequences of mappings, flow
// sequences, and plain or single-quoted scalars. A TBD file for a framework
// can be megabytes of symbol lists, and going through the general YAML
// parser and YAMLTraits for those takes several times as long as this does.
// Anything this does not handle makes it give up, and the general reader
// takes over -- so this never has to report an error itself.
class TBDv4FastReader {
public:
  TBDv4FastReader(StringRef Buffer, StringRef Path, bool SkipUnknownTriples)
      : Buf(Buffer), Path(Path), SkipUnknownTriples(SkipUnknownTriples) {}

  // The file, or null if the input is not in the handled subset.
  std::unique_ptr<InterfaceFile> read() {
    std::unique_ptr<InterfaceFile> First;
    while (true) {
      size_t Indent;
      if (!nextContentLine(Indent) || Indent != 0 ||
          restOfLine().rtrim(' ') != "--- !tapi-tbd")
        return nullptr;
      skipLine();
      std::unique_ptr<InterfaceFile> Doc = readDocument();
      if (!Doc)
        return nullptr;
      // readDocument() stops at the "..." line.
      skipLine();
      if (First)
        First->addDocument(std::move(Doc));
      else
        First = std::move(Doc);
      if (!nextContentLine(Indent))
        return First;
    }
  }

private:
  StringRef Buf;
  size_t Pos = 0;
  StringRef Path;
  bool SkipUnknownTriples;
  // Unescaped single-quoted scalars; a deque so that references stay valid.
  std::deque<std::string> Storage;

  struct Section {
    TargetList Targets;
    bool HasTargets = false;
    std::vector<StringRef> Symbols, Classes, ClassEHs, Ivars, WeakSymbols,
        TlvSymbols;
  };
  struct Metadata {
    TargetList Targets;
    bool HasTargets = false;
    std::vector<StringRef> Values;
    bool HasValues = false;
  };
  struct Umbrella {
    TargetList Targets;
    bool HasTargets = false;
    std::optional<StringRef> Name;
  };

  // With Pos at the start of a line: skips blank and comment lines, and
  // leaves Pos at the start of the next line with content, whose
  // indentation goes in Indent. False at the end of the input.
  bool nextContentLine(size_t &Indent) {
    while (Pos < Buf.size()) {
      size_t I = Pos;
      while (I < Buf.size() && Buf[I] == ' ')
        ++I;
      if (I < Buf.size() && Buf[I] != '\n' && Buf[I] != '\r' && Buf[I] != '#') {
        Indent = I - Pos;
        return true;
      }
      skipLine();
    }
    return false;
  }
  StringRef restOfLine() const {
    size_t End = Buf.find('\n', Pos);
    return Buf.slice(Pos, End == StringRef::npos ? Buf.size() : End);
  }
  void skipLine() {
    size_t End = Buf.find('\n', Pos);
    Pos = End == StringRef::npos ? Buf.size() : End + 1;
  }
  void skipSpaces() {
    while (Pos < Buf.size() && Buf[Pos] == ' ')
      ++Pos;
  }
  // The rest of the line must be blank; consumes it.
  bool endLine() {
    skipSpaces();
    if (Pos < Buf.size() && Buf[Pos] == '\r')
      ++Pos;
    if (Pos < Buf.size() && Buf[Pos] != '\n')
      return false;
    if (Pos < Buf.size())
      ++Pos;
    return true;
  }

  // Reads "key:" at Pos, leaving Pos after the colon and any spaces.
  bool readKey(StringRef &Key) {
    size_t I = Pos;
    while (I < Buf.size() &&
           (isAlnum(Buf[I]) || Buf[I] == '-' || Buf[I] == '_'))
      ++I;
    if (I == Pos || I >= Buf.size() || Buf[I] != ':')
      return false;
    if (I + 1 < Buf.size() && Buf[I + 1] != ' ' && Buf[I + 1] != '\n' &&
        Buf[I + 1] != '\r')
      return false;
    Key = Buf.slice(Pos, I);
    Pos = I + 1;
    skipSpaces();
    return true;
  }

  // Whether the line is at its end (or a comment) at Pos.
  bool atLineEnd() const {
    return Pos >= Buf.size() || Buf[Pos] == '\n' || Buf[Pos] == '\r';
  }

  // A scalar, plain or single-quoted, ending at the end of the line or, in
  // a flow sequence, at ',' or ']'.
  bool readScalar(StringRef &Value, bool InFlow) {
    if (atLineEnd())
      return false;
    if (Buf[Pos] == '\'') {
      size_t Start = ++Pos;
      std::string *Unescaped = nullptr;
      while (true) {
        if (Pos >= Buf.size() || Buf[Pos] == '\n' || Buf[Pos] == '\r')
          return false;
        if (Buf[Pos] == '\'') {
          if (Pos + 1 < Buf.size() && Buf[Pos + 1] == '\'') {
            if (!Unescaped) {
              Storage.emplace_back(Buf.slice(Start, Pos));
              Unescaped = &Storage.back();
            } else {
              *Unescaped += '\'';
            }
            Pos += 2;
            continue;
          }
          break;
        }
        if (Unescaped)
          *Unescaped += Buf[Pos];
        ++Pos;
      }
      Value = Unescaped ? StringRef(*Unescaped) : Buf.slice(Start, Pos);
      ++Pos; // the closing quote
      skipSpaces();
      return true;
    }
    // Plain: no indicators, no comments, no mapping separators inside.
    if (StringRef("-?:,[]{}#&*!|>'\"%@`").contains(Buf[Pos]))
      return false;
    size_t Start = Pos;
    while (Pos < Buf.size()) {
      char C = Buf[Pos];
      if (C == '\n' || C == '\r' || C == '\t' || C == '#' || C == '\'' ||
          C == '"' || C == '{' || C == '}' || C == '[' || C == ']')
        break;
      if (InFlow && C == ',')
        break;
      if (C == ':' && (Pos + 1 == Buf.size() || Buf[Pos + 1] == ' ' ||
                       Buf[Pos + 1] == '\n' || Buf[Pos + 1] == '\r'))
        break;
      ++Pos;
    }
    if (Pos < Buf.size() &&
        (Buf[Pos] == '#' || Buf[Pos] == '\'' || Buf[Pos] == '"' ||
         Buf[Pos] == '{' || Buf[Pos] == '}' || Buf[Pos] == '[' ||
         Buf[Pos] == '\t' || Buf[Pos] == ':'))
      return false;
    if (!InFlow && Pos < Buf.size() && Buf[Pos] == ']')
      return false;
    Value = Buf.slice(Start, Pos).rtrim(' ');
    if (Value.empty())
      return false;
    return true;
  }

  // "[ a, b, c ]", possibly over several lines. Pos is at the '['.
  bool readFlowSequence(std::vector<StringRef> &Values) {
    if (Pos >= Buf.size() || Buf[Pos] != '[')
      return false;
    ++Pos;
    while (true) {
      // Whitespace and line breaks between the items.
      while (Pos < Buf.size() &&
             (Buf[Pos] == ' ' || Buf[Pos] == '\n' || Buf[Pos] == '\r'))
        ++Pos;
      if (Pos >= Buf.size())
        return false;
      if (Buf[Pos] == ']') {
        ++Pos;
        skipSpaces();
        return true;
      }
      StringRef Value;
      if (!readScalar(Value, /*InFlow=*/true))
        return false;
      Values.push_back(Value);
      while (Pos < Buf.size() &&
             (Buf[Pos] == ' ' || Buf[Pos] == '\n' || Buf[Pos] == '\r'))
        ++Pos;
      if (Pos >= Buf.size())
        return false;
      if (Buf[Pos] == ',') {
        ++Pos;
        continue;
      }
      if (Buf[Pos] != ']')
        return false;
    }
  }

  // The value after "key: " when it is inline: a scalar or a flow sequence.
  bool readInlineScalar(StringRef &Value) {
    return readScalar(Value, /*InFlow=*/false) && endLine();
  }
  bool readInlineSequence(std::vector<StringRef> &Values) {
    return readFlowSequence(Values) && endLine();
  }

  bool readTargets(TargetList &Targets) {
    std::vector<StringRef> Values;
    if (!readInlineSequence(Values))
      return false;
    for (StringRef V : Values) {
      Expected<Target> T = Target::create(V);
      if (!T) {
        consumeError(T.takeError());
        return false;
      }
      // See ScalarTraits<Target>::input(): an unknown target is an error
      // unless they are to be skipped, in which case it is dropped.
      if (!T->isValid()) {
        if (!SkipUnknownTriples)
          return false;
        continue;
      }
      Targets.push_back(*T);
    }
    return true;
  }

  // A block sequence of mappings whose values are all inline:
  //   - key: value
  //     key: value
  // Pos is at the start of the line after "key:". Calls Item(Key) with Pos
  // after "key: " for each key; Item returns false to give up.
  bool readBlockSequence(size_t ParentIndent, function_ref<bool()> BeginItem,
                         function_ref<bool(StringRef)> Item) {
    size_t Indent;
    if (!nextContentLine(Indent) || Indent <= ParentIndent)
      return false;
    size_t ItemIndent = Indent;
    while (true) {
      if (!nextContentLine(Indent) || Indent <= ParentIndent)
        return true;
      if (Indent != ItemIndent)
        return false;
      Pos += Indent;
      if (!Buf.substr(Pos).starts_with("- "))
        return false;
      Pos += 2;
      if (!BeginItem())
        return false;
      size_t KeyIndent = ItemIndent + 2;
      // The first key is on the "- " line.
      while (true) {
        StringRef Key;
        if (!readKey(Key) || !Item(Key))
          return false;
        if (!nextContentLine(Indent))
          return true;
        if (Indent != KeyIndent)
          break;
        Pos += Indent;
        if (Buf.substr(Pos).starts_with("- "))
          return false;
      }
    }
  }

  bool readMetadataSequence(std::vector<Metadata> &Out, StringRef ValuesKey) {
    return readBlockSequence(
        0,
        [&] {
          Out.emplace_back();
          return true;
        },
        [&](StringRef Key) {
          Metadata &M = Out.back();
          if (Key == "targets" && !M.HasTargets) {
            M.HasTargets = true;
            return readTargets(M.Targets);
          }
          if (Key == ValuesKey && !M.HasValues) {
            M.HasValues = true;
            return readInlineSequence(M.Values);
          }
          return false;
        });
  }

  bool readSymbolSequence(std::vector<Section> &Out) {
    return readBlockSequence(
        0,
        [&] {
          Out.emplace_back();
          return true;
        },
        [&](StringRef Key) {
          Section &S = Out.back();
          if (Key == "targets") {
            if (S.HasTargets)
              return false;
            S.HasTargets = true;
            return readTargets(S.Targets);
          }
          std::vector<StringRef> *List =
              StringSwitch<std::vector<StringRef> *>(Key)
                  .Case("symbols", &S.Symbols)
                  .Case("objc-classes", &S.Classes)
                  .Case("objc-eh-types", &S.ClassEHs)
                  .Case("objc-ivars", &S.Ivars)
                  .Case("weak-symbols", &S.WeakSymbols)
                  .Case("thread-local-symbols", &S.TlvSymbols)
                  .Default(nullptr);
          if (!List || !List->empty())
            return false;
          return readInlineSequence(*List);
        });
  }

  std::unique_ptr<InterfaceFile> readDocument() {
    std::optional<unsigned> TBDVersion;
    std::optional<TargetList> Targets;
    std::optional<StringRef> InstallName;
    PackedVersion CurrentVersion(1, 0, 0), CompatibilityVersion(1, 0, 0);
    SwiftVersion SwiftABIVersion = 0;
    TBDFlags Flags = TBDFlags::None;
    std::vector<Umbrella> ParentUmbrellas;
    std::vector<Metadata> AllowableClients, ReexportedLibraries;
    std::vector<Section> Exports, Reexports, Undefineds;
    SmallVector<StringRef, 16> SeenKeys;

    while (true) {
      size_t Indent;
      if (!nextContentLine(Indent) || Indent != 0)
        return nullptr;
      if (restOfLine().rtrim(' ') == "...")
        break;
      StringRef Key;
      if (!readKey(Key))
        return nullptr;
      // A mapping does not repeat a key.
      if (is_contained(SeenKeys, Key))
        return nullptr;
      SeenKeys.push_back(Key);
      StringRef Value;
      bool Ok;
      if (Key == "tbd-version") {
        unsigned V;
        Ok = readInlineScalar(Value) && !Value.getAsInteger(10, V);
        TBDVersion = V;
      } else if (Key == "targets") {
        Targets.emplace();
        Ok = readTargets(*Targets);
      } else if (Key == "uuids") {
        // Parsed and ignored, as by the general reader.
        Ok = endLine() && readBlockSequence(
                              0, [] { return true; },
                              [&](StringRef K) {
                                return (K == "target" || K == "value") &&
                                       readInlineScalar(Value);
                              });
      } else if (Key == "flags") {
        std::vector<StringRef> Names;
        Ok = readInlineSequence(Names);
        for (StringRef Name : Names) {
          TBDFlags Flag = StringSwitch<TBDFlags>(Name)
                              .Case("flat_namespace", TBDFlags::FlatNamespace)
                              .Case("not_app_extension_safe",
                                    TBDFlags::NotApplicationExtensionSafe)
                              .Case("installapi", TBDFlags::InstallAPI)
                              .Case("not_for_dyld_shared_cache",
                                    TBDFlags::OSLibNotForSharedCache)
                              .Default(TBDFlags::None);
          if (Flag == TBDFlags::None)
            return nullptr;
          Flags |= Flag;
        }
      } else if (Key == "install-name") {
        Ok = readInlineScalar(Value);
        InstallName = Value;
      } else if (Key == "current-version") {
        Ok = readInlineScalar(Value) && CurrentVersion.parse32(Value);
      } else if (Key == "compatibility-version") {
        Ok = readInlineScalar(Value) && CompatibilityVersion.parse32(Value);
      } else if (Key == "swift-abi-version") {
        Ok =
            readInlineScalar(Value) && !Value.getAsInteger(10, SwiftABIVersion);
      } else if (Key == "parent-umbrella") {
        Ok = endLine() && readBlockSequence(
                              0,
                              [&] {
                                ParentUmbrellas.emplace_back();
                                return true;
                              },
                              [&](StringRef K) {
                                Umbrella &U = ParentUmbrellas.back();
                                if (K == "targets" && !U.HasTargets) {
                                  U.HasTargets = true;
                                  return readTargets(U.Targets);
                                }
                                if (K == "umbrella" && !U.Name) {
                                  StringRef Name;
                                  if (!readInlineScalar(Name))
                                    return false;
                                  U.Name = Name;
                                  return true;
                                }
                                return false;
                              });
        for (const Umbrella &U : ParentUmbrellas)
          if (!U.HasTargets || !U.Name)
            return nullptr;
      } else if (Key == "allowable-clients") {
        Ok = endLine() && readMetadataSequence(AllowableClients, "clients");
      } else if (Key == "reexported-libraries") {
        Ok =
            endLine() && readMetadataSequence(ReexportedLibraries, "libraries");
      } else if (Key == "exports") {
        Ok = endLine() && readSymbolSequence(Exports);
      } else if (Key == "reexports") {
        Ok = endLine() && readSymbolSequence(Reexports);
      } else if (Key == "undefineds") {
        Ok = endLine() && readSymbolSequence(Undefineds);
      } else {
        return nullptr;
      }
      if (!Ok)
        return nullptr;
    }
    if (!TBDVersion || !Targets || !InstallName)
      return nullptr;
    for (const std::vector<Metadata> *List :
         {&AllowableClients, &ReexportedLibraries})
      for (const Metadata &M : *List)
        if (!M.HasTargets || !M.HasValues)
          return nullptr;
    for (const std::vector<Section> *List : {&Exports, &Reexports, &Undefineds})
      for (const Section &S : *List)
        if (!S.HasTargets)
          return nullptr;

    // From here on this mirrors NormalizedTBD_V4::denormalize().
    auto File = std::make_unique<InterfaceFile>();
    File->setPath(Path);
    File->setFileType(FileType::TBD_V4);
    File->addTargets(*Targets);
    File->setInstallName(*InstallName);
    File->setCurrentVersion(CurrentVersion);
    File->setCompatibilityVersion(CompatibilityVersion);
    File->setSwiftABIVersion(SwiftABIVersion);
    for (const Umbrella &U : ParentUmbrellas)
      for (const Target &T : U.Targets)
        File->addParentUmbrella(T, *U.Name);
    File->setTwoLevelNamespace(!(Flags & TBDFlags::FlatNamespace));
    File->setApplicationExtensionSafe(
        !(Flags & TBDFlags::NotApplicationExtensionSafe));
    File->setOSLibNotForSharedCache((Flags & TBDFlags::OSLibNotForSharedCache));
    for (const Metadata &M : AllowableClients)
      for (StringRef Lib : M.Values)
        for (const Target &T : M.Targets)
          File->addAllowableClient(Lib, T);
    for (const Metadata &M : ReexportedLibraries)
      for (StringRef Lib : M.Values)
        for (const Target &T : M.Targets)
          File->addReexportedLibrary(Lib, T);

    auto HandleSymbols = [&File](const std::vector<Section> &Sections,
                                 SymbolFlags InputFlag = SymbolFlags::None) {
      const SymbolFlags Flag = InputFlag | SymbolFlags::Data;
      for (const Section &S : Sections) {
        if (S.Targets.empty())
          continue;
        for (StringRef Sym : S.Symbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Sym, S.Targets, Flag);
        for (StringRef Sym : S.Classes)
          File->addSymbol(EncodeKind::ObjectiveCClass, Sym, S.Targets, Flag);
        for (StringRef Sym : S.ClassEHs)
          File->addSymbol(EncodeKind::ObjectiveCClassEHType, Sym, S.Targets,
                          Flag);
        for (StringRef Sym : S.Ivars)
          File->addSymbol(EncodeKind::ObjectiveCInstanceVariable, Sym,
                          S.Targets, Flag);
        SymbolFlags SymFlag =
            ((Flag & SymbolFlags::Undefined) == SymbolFlags::Undefined)
                ? SymbolFlags::WeakReferenced
                : SymbolFlags::WeakDefined;
        for (StringRef Sym : S.WeakSymbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Sym, S.Targets,
                          Flag | SymFlag);
        for (StringRef Sym : S.TlvSymbols)
          File->addSymbol(EncodeKind::GlobalSymbol, Sym, S.Targets,
                          Flag | SymbolFlags::ThreadLocalValue);
      }
    };
    HandleSymbols(Exports);
    HandleSymbols(Reexports, SymbolFlags::Rexported);
    HandleSymbols(Undefineds, SymbolFlags::Undefined);
    return File;
  }
};

} // end anonymous namespace

Expected<std::unique_ptr<InterfaceFile>>
TextAPIReader::get(MemoryBufferRef InputBuffer, bool SkipUnknownTriples) {
  TextAPIContext Ctx;

  Ctx.SkipUnknownTriples = SkipUnknownTriples;
  Ctx.Path = std::string(InputBuffer.getBufferIdentifier());
  if (auto FTOrErr = canRead(InputBuffer))
    Ctx.FileKind = *FTOrErr;
  else
    return FTOrErr.takeError();

  // Handle JSON Format.
  if (Ctx.FileKind >= FileType::TBD_V5) {
    auto FileOrErr = getInterfaceFileFromJSON(InputBuffer.getBuffer());
    if (!FileOrErr)
      return FileOrErr.takeError();

    (*FileOrErr)->setPath(Ctx.Path);
    return std::move(*FileOrErr);
  }
  if (Ctx.FileKind == FileType::TBD_V4)
    if (std::unique_ptr<InterfaceFile> File =
            TBDv4FastReader(InputBuffer.getBuffer(), Ctx.Path,
                            SkipUnknownTriples)
                .read())
      return std::move(File);

  yaml::Input YAMLIn(InputBuffer.getBuffer(), &Ctx, DiagHandler, &Ctx);

  // Fill vector with interface file objects created by parsing the YAML file.
  std::vector<const InterfaceFile *> Files;
  YAMLIn >> Files;

  // YAMLIn dynamically allocates for Interface file and in case of error,
  // memory leak will occur unless wrapped around unique_ptr
  auto File = std::unique_ptr<InterfaceFile>(
      const_cast<InterfaceFile *>(Files.front()));

  for (const InterfaceFile *FI : llvm::drop_begin(Files))
    File->addDocument(
        std::shared_ptr<InterfaceFile>(const_cast<InterfaceFile *>(FI)));

  if (YAMLIn.error())
    return make_error<StringError>(Ctx.ErrorMessage, YAMLIn.error());

  return std::move(File);
}

Error TextAPIWriter::writeToStream(raw_ostream &OS, const InterfaceFile &File,
                                   const FileType FileKind, bool Compact) {
  TextAPIContext Ctx;
  Ctx.Path = std::string(File.getPath());

  // Prefer parameter for format if passed, otherwise fallback to the File
  // FileType.
  Ctx.FileKind =
      (FileKind == FileType::Invalid) ? File.getFileType() : FileKind;

  // Write out in JSON format.
  if (Ctx.FileKind >= FileType::TBD_V5) {
    return serializeInterfaceFileToJSON(OS, File, Ctx.FileKind, Compact);
  }

  llvm::yaml::Output YAMLOut(OS, &Ctx, /*WrapColumn=*/80);

  std::vector<const InterfaceFile *> Files;
  Files.emplace_back(&File);

  for (const auto &Document : File.documents())
    Files.emplace_back(Document.get());

  // Stream out yaml.
  YAMLOut << Files;

  return Error::success();
}
