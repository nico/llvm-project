//===- ObjectFile.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ObjectFile.h"
#include "MachOFile.h"
#include "dsymutil2.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Object/Archive.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::dsymutil2;

namespace {
/// A static library that some object files of the debug map are members of.
struct ArchiveFile {
  std::string Path;
  std::unique_ptr<MemoryBuffer> Buffer;
  std::string Error;
  /// The members of a thin archive are files next to the archive.
  bool IsThin = false;
  /// The members of a regular archive, by name.
  StringMap<StringRef> Members;
};
} // namespace

static std::unique_ptr<MemoryBuffer> openFile(const Twine &Path,
                                              std::string &Error) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer = MemoryBuffer::getFile(
      Path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (std::error_code EC = Buffer.getError()) {
    Error = EC.message();
    return nullptr;
  }
  return std::move(*Buffer);
}

static void openArchive(ArchiveFile &File) {
  File.Buffer = openFile(File.Path, File.Error);
  if (!File.Buffer)
    return;
  Error Err = Error::success();
  object::Archive Archive(File.Buffer->getMemBufferRef(), Err);
  if (Err) {
    File.Error = toString(std::move(Err));
    return;
  }
  File.IsThin = Archive.isThin();
  if (File.IsThin)
    return;
  for (const object::Archive::Child &Child : Archive.children(Err)) {
    Expected<StringRef> Name = Child.getName();
    Expected<StringRef> Contents = Name ? Child.getBuffer() : StringRef();
    if (!Name || !Contents) {
      File.Error = toString(Name ? Contents.takeError() : Name.takeError());
      return;
    }
    // If two members have the same name, the first one wins.
    File.Members.try_emplace(*Name, *Contents);
  }
  if (Err)
    File.Error = toString(std::move(Err));
}

/// Finds the DWARF sections of \p Contents and the addresses of the debug
/// map's symbols.
static void readObject(ObjectFile &Object, StringRef Contents) {
  MachOFile File;
  Object.Warning = File.parse(Contents);
  if (!Object.Warning.empty())
    return;
  Object.Info = File.sectionContents("__DWARF", "__debug_info");
  Object.Abbrev = File.sectionContents("__DWARF", "__debug_abbrev");
  Object.Str = File.sectionContents("__DWARF", "__debug_str");
  Object.Line = File.sectionContents("__DWARF", "__debug_line");
  Object.Ranges = File.sectionContents("__DWARF", "__debug_ranges");
  Object.Frame = File.sectionContents("__DWARF", "__debug_frame");

  const std::vector<MapSymbol> &Symbols = Object.Map->Symbols;
  DenseMap<StringRef, uint32_t> SymbolsByName;
  SymbolsByName.reserve(Symbols.size());
  for (uint32_t I = 0; I != Symbols.size(); ++I)
    SymbolsByName.try_emplace(Symbols[I].Name, I);

  Object.SymbolAddresses.assign(Symbols.size(), ObjectFile::NotFound);
  for (uint32_t I = 0; I != File.NumSymbols; ++I) {
    MachOSymbol S = File.symbol(I);
    if (!S.isDefinedInSection())
      continue;
    auto It = SymbolsByName.find(S.Name);
    if (It != SymbolsByName.end())
      Object.SymbolAddresses[It->second] = S.Value;
  }

  // The debug map sometimes has a function without a size. Such a function
  // is taken to end where the next symbol of the object file starts.
  std::vector<uint64_t> AllAddresses;
  auto SizeFromNextSymbol = [&](uint64_t Address) -> uint64_t {
    if (AllAddresses.empty()) {
      for (uint32_t I = 0; I != File.NumSymbols; ++I) {
        MachOSymbol S = File.symbol(I);
        if (S.isDefinedInSection())
          AllAddresses.push_back(S.Value);
      }
      llvm::sort(AllAddresses);
    }
    auto Next = llvm::upper_bound(AllAddresses, Address);
    return Next == AllAddresses.end() ? 0 : *Next - Address;
  };

  uint32_t NumMissing = 0;
  StringRef FirstMissing;
  for (uint32_t I = 0; I != Symbols.size(); ++I) {
    uint64_t Address = Object.SymbolAddresses[I];
    if (Address == ObjectFile::NotFound) {
      if (NumMissing++ == 0)
        FirstMissing = Symbols[I].Name;
      continue;
    }
    uint64_t Size = Symbols[I].Size;
    if (Size == 0 && Symbols[I].Kind == MapSymbol::Function)
      Size = SizeFromNextSymbol(Address);
    Object.Addresses.push_back({Address, Address + Size, Symbols[I].Address});
  }
  llvm::sort(Object.Addresses,
             [](const AddressRange &A, const AddressRange &B) {
               return std::tie(A.Begin, B.End) < std::tie(B.Begin, A.End);
             });
  // Of symbols with the same address, keep the largest.
  Object.Addresses.erase(
      llvm::unique(Object.Addresses,
                   [](const AddressRange &A, const AddressRange &B) {
                     return A.Begin == B.Begin;
                   }),
      Object.Addresses.end());
  if (NumMissing)
    Object.Warning = (Twine("could not find symbol '") + FirstMissing + "'" +
                      (NumMissing > 1 ? " and " + Twine(NumMissing - 1) +
                                            " more"
                                      : Twine()))
                         .str();
}

/// Calls \p Fn(I) for all I below \p Size, on few threads. Opening and mapping
/// files is serialized by the kernel: on macOS it is fastest with about four
/// threads and several times slower with sixteen.
template <class FnTy> static void parallelForOpen(size_t Size, FnTy Fn) {
  const size_t NumTasks = 4;
  parallelFor(0, NumTasks, [&](size_t Task) {
    for (size_t I = Task; I < Size; I += NumTasks)
      Fn(I);
  });
}

/// The path of the debug map's \p Path on disk.
static std::string pathOnDisk(StringRef PrependPath, StringRef Path) {
  if (PrependPath.empty())
    return Path.str();
  SmallString<256> Result = PrependPath;
  sys::path::append(Result, Path);
  return std::string(Result);
}

InputFiles dsymutil2::openObjectFiles(const DebugMap &Map,
                                      StringRef PrependPath) {
  // An object file is "path" or "archive(member)". Find the archives first,
  // so that each is opened once.
  struct ObjectPath {
    StringRef Member;
    uint32_t Archive;
  };
  std::vector<ObjectPath> Paths(Map.Objects.size());
  std::vector<ArchiveFile> Archives;
  StringMap<uint32_t> ArchiveIndices;
  for (size_t I = 0; I != Map.Objects.size(); ++I) {
    StringRef Path = Map.Objects[I].Path;
    size_t Paren = Path.ends_with(")") ? Path.rfind('(') : StringRef::npos;
    if (Paren == StringRef::npos)
      continue;
    auto [It, Inserted] =
        ArchiveIndices.try_emplace(Path.substr(0, Paren), Archives.size());
    if (Inserted)
      Archives.emplace_back().Path = pathOnDisk(PrependPath, It->first());
    Paths[I] = {Path.slice(Paren + 1, Path.size() - 1), It->second};
  }
  {
    PhaseTimer Timer("open archives");
    parallelForOpen(Archives.size(),
                    [&](size_t I) { openArchive(Archives[I]); });
  }

  InputFiles Inputs;
  Inputs.Objects.resize(Map.Objects.size());
  std::vector<StringRef> Contents(Map.Objects.size());
  {
    PhaseTimer Timer("open objects");
    parallelForOpen(Inputs.Objects.size(), [&](size_t I) {
      ObjectFile &Object = Inputs.Objects[I];
      Object.Map = &Map.Objects[I];
      std::string Path;
      if (Paths[I].Member.empty()) {
        Path = pathOnDisk(PrependPath, Object.Map->Path);
      } else {
        const ArchiveFile &Archive = Archives[Paths[I].Archive];
        if (!Archive.Error.empty()) {
          Object.Warning = Archive.Error;
          return;
        }
        if (!Archive.IsThin) {
          Contents[I] = Archive.Members.lookup(Paths[I].Member);
          if (Contents[I].empty())
            Object.Warning = "no such member in the archive";
          return;
        }
        SmallString<256> MemberPath = sys::path::parent_path(Archive.Path);
        sys::path::append(MemberPath, Paths[I].Member);
        Path = std::string(MemberPath);
      }
      Object.Buffer = openFile(Path, Object.Warning);
      if (Object.Buffer)
        Contents[I] = Object.Buffer->getBuffer();
    });
  }
  {
    PhaseTimer Timer("match symbols");
    parallelFor(0, Inputs.Objects.size(), [&](size_t I) {
      if (Inputs.Objects[I].Warning.empty())
        readObject(Inputs.Objects[I], Contents[I]);
    });
  }

  for (ArchiveFile &Archive : Archives)
    if (Archive.Buffer && !Archive.IsThin)
      Inputs.Archives.push_back(std::move(Archive.Buffer));
  return Inputs;
}

const AddressRange *ObjectFile::findContaining(uint64_t Address) const {
  auto It = llvm::upper_bound(Addresses, Address,
                              [](uint64_t Address, const AddressRange &Range) {
                                return Address < Range.Begin;
                              });
  if (It == Addresses.begin())
    return nullptr;
  --It;
  // A range without a known size contains only its start.
  return Address < It->End || Address == It->Begin ? &*It : nullptr;
}

const AddressRange *ObjectFile::findStart(uint64_t Address) const {
  const AddressRange *Range = findContaining(Address);
  return Range && Range->Begin == Address ? Range : nullptr;
}

void dsymutil2::dumpDebugMap(const std::vector<ObjectFile> &Objects,
                             raw_ostream &OS) {
  static const char *const KindNames[] = {"function", "global", "static"};
  for (const ObjectFile &Object : Objects) {
    OS << "object " << Object.Map->Path << '\n';
    for (size_t I = 0; I != Object.Map->Symbols.size(); ++I) {
      const MapSymbol &Symbol = Object.Map->Symbols[I];
      OS << "  " << KindNames[Symbol.Kind] << ' ' << Symbol.Name << ": ";
      if (I < Object.SymbolAddresses.size() &&
          Object.SymbolAddresses[I] != ObjectFile::NotFound)
        OS << format_hex(Object.SymbolAddresses[I], 1);
      else
        OS << "not found";
      OS << " -> " << format_hex(Symbol.Address, 1);
      if (Symbol.Kind == MapSymbol::Function)
        OS << ", size " << format_hex(Symbol.Size, 1);
      OS << '\n';
    }
  }
}
