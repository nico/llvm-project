//===- DebugMap.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DebugMap.h"
#include "dsymutil2.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

namespace {
/// The fields of a nlist_64, the symbol table entry of 64-bit Mach-O files.
struct Symbol {
  StringRef Name;
  uint8_t Type;
  uint8_t Section;
  uint64_t Value;
};

class Reader {
public:
  explicit Reader(MemoryBufferRef Binary)
      : Name(Binary.getBufferIdentifier()), Data(Binary.getBuffer()) {}

  DebugMap read();

private:
  /// Returns \p Size bytes at \p Offset, or reports the file as truncated.
  const char *bytes(uint64_t Offset, uint64_t Size) {
    if (Offset > Data.size() || Size > Data.size() - Offset)
      fatal(Name + ": truncated or malformed Mach-O file");
    return Data.data() + Offset;
  }

  void readLoadCommands();
  Symbol symbol(uint32_t Index);
  uint64_t addressOfGlobal(StringRef SymbolName);

  StringRef Name;
  StringRef Data;
  MachO::symtab_command Symtab = {};
  /// Whether section N (they are numbered from 1) holds code.
  std::vector<bool> IsCodeSection = {false};
  /// Addresses of the binary's regular symbols by name, to resolve N_GSYM
  /// stabs, which carry no address. Built on first use.
  DenseMap<StringRef, uint64_t> GlobalAddresses;
  bool HaveGlobalAddresses = false;
};
} // namespace

void Reader::readLoadCommands() {
  const char *Header = bytes(0, sizeof(MachO::mach_header_64));
  uint32_t Magic = read32le(Header);
  if (Magic == MachO::FAT_MAGIC || Magic == MachO::FAT_CIGAM ||
      Magic == MachO::FAT_MAGIC_64 || Magic == MachO::FAT_CIGAM_64)
    fatal(Name + ": universal binaries are not supported");
  if (Magic != MachO::MH_MAGIC_64)
    fatal(Name + ": not a 64-bit little-endian Mach-O file");
  uint32_t NumCommands = read32le(Header + 16);

  uint64_t Offset = sizeof(MachO::mach_header_64);
  for (uint32_t I = 0; I != NumCommands; ++I) {
    const char *Command = bytes(Offset, sizeof(MachO::load_command));
    uint32_t Kind = read32le(Command);
    uint32_t Size = read32le(Command + 4);
    if (Size < sizeof(MachO::load_command))
      fatal(Name + ": malformed load command");
    bytes(Offset, Size);
    if (Kind == MachO::LC_SYMTAB &&
        Size >= sizeof(MachO::symtab_command)) {
      Symtab.symoff = read32le(Command + 8);
      Symtab.nsyms = read32le(Command + 12);
      Symtab.stroff = read32le(Command + 16);
      Symtab.strsize = read32le(Command + 20);
    } else if (Kind == MachO::LC_SEGMENT_64 &&
               Size >= sizeof(MachO::segment_command_64)) {
      uint32_t NumSections = read32le(Command + 64);
      if (uint64_t(NumSections) * sizeof(MachO::section_64) >
          Size - sizeof(MachO::segment_command_64))
        fatal(Name + ": malformed segment load command");
      const char *Section = Command + sizeof(MachO::segment_command_64);
      for (uint32_t J = 0; J != NumSections; ++J) {
        uint32_t Flags = read32le(Section + 64);
        IsCodeSection.push_back(Flags & (MachO::S_ATTR_PURE_INSTRUCTIONS |
                                         MachO::S_ATTR_SOME_INSTRUCTIONS));
        Section += sizeof(MachO::section_64);
      }
    }
    Offset += Size;
  }
  bytes(Symtab.symoff, uint64_t(Symtab.nsyms) * sizeof(MachO::nlist_64));
  bytes(Symtab.stroff, Symtab.strsize);
}

Symbol Reader::symbol(uint32_t Index) {
  const char *Entry =
      Data.data() + Symtab.symoff + uint64_t(Index) * sizeof(MachO::nlist_64);
  uint32_t NameOffset = read32le(Entry);
  if (NameOffset >= Symtab.strsize)
    fatal(Name + ": symbol name outside of the string table");
  // The name ends at the first NUL, or at the end of the string table.
  StringRef Strings = Data.substr(Symtab.stroff, Symtab.strsize);
  StringRef SymbolName = Strings.substr(NameOffset);
  SymbolName = SymbolName.substr(0, SymbolName.find('\0'));
  return {SymbolName, uint8_t(Entry[4]), uint8_t(Entry[5]),
          read64le(Entry + 8)};
}

uint64_t Reader::addressOfGlobal(StringRef SymbolName) {
  if (!HaveGlobalAddresses) {
    HaveGlobalAddresses = true;
    for (uint32_t I = 0; I != Symtab.nsyms; ++I) {
      Symbol S = symbol(I);
      if ((S.Type & MachO::N_STAB) ||
          (S.Type & MachO::N_TYPE) != MachO::N_SECT)
        continue;
      // Globals are external or, after the linker has hidden them, local
      // symbols in data sections. An external symbol wins over a local one of
      // the same name.
      bool IsExternal = S.Type & MachO::N_EXT;
      bool IsCode = S.Section < IsCodeSection.size() && IsCodeSection[S.Section];
      if (IsExternal)
        GlobalAddresses[S.Name] = S.Value;
      else if (!IsCode)
        GlobalAddresses.try_emplace(S.Name, S.Value);
    }
  }
  return GlobalAddresses.lookup(SymbolName);
}

DebugMap Reader::read() {
  readLoadCommands();

  DebugMap Map;
  MapObject *Object = nullptr;
  for (uint32_t I = 0; I != Symtab.nsyms; ++I) {
    Symbol S = symbol(I);
    if (S.Type == MachO::N_OSO) {
      Object = &Map.Objects.emplace_back();
      Object->Path = S.Name;
      continue;
    }
    if (!Object)
      continue;
    switch (S.Type) {
    case MachO::N_SO:
      // The end of the stabs of one object file.
      if (S.Name.empty())
        Object = nullptr;
      break;
    case MachO::N_FUN:
      // A function has two entries: its name and address, then an entry
      // without a name that holds its size.
      if (!S.Name.empty())
        Object->Symbols.push_back(
            {S.Name, S.Value, 0, MapSymbol::Function});
      else if (!Object->Symbols.empty() &&
               Object->Symbols.back().Kind == MapSymbol::Function)
        Object->Symbols.back().Size = S.Value;
      break;
    case MachO::N_GSYM:
      if (uint64_t Address = addressOfGlobal(S.Name))
        Object->Symbols.push_back({S.Name, Address, 0, MapSymbol::Global});
      break;
    case MachO::N_STSYM:
      Object->Symbols.push_back({S.Name, S.Value, 0, MapSymbol::Static});
      break;
    }
  }
  return Map;
}

DebugMap dsymutil2::readDebugMap(MemoryBufferRef Binary) {
  return Reader(Binary).read();
}

void dsymutil2::dumpDebugMap(const DebugMap &Map, raw_ostream &OS) {
  static const char *const KindNames[] = {"function", "global", "static"};
  for (const MapObject &Object : Map.Objects) {
    OS << "object " << Object.Path << '\n';
    for (const MapSymbol &Symbol : Object.Symbols) {
      OS << "  " << KindNames[Symbol.Kind] << ' ' << Symbol.Name << ' '
         << format_hex(Symbol.Address, 1);
      if (Symbol.Kind == MapSymbol::Function)
        OS << ' ' << format_hex(Symbol.Size, 1);
      OS << '\n';
    }
  }
}
