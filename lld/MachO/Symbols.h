//===- Symbols.h ------------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_SYMBOLS_H
#define LLD_MACHO_SYMBOLS_H

#include "InputSection.h"
#include "lld/Common/Strings.h"
#include "llvm/Object/Archive.h"

namespace lld {
namespace macho {

class InputSection;
class InputFile;
class ArchiveFile;

enum {
  PageSize = 4096,
  ImageBase = 4096,
};

struct StringRefZ {
  StringRefZ(const char *S) : Data(S), Size(-1) {}
  StringRefZ(StringRef S) : Data(S.data()), Size(S.size()) {}

  const char *Data;
  const uint32_t Size;
};

class Symbol {
public:
  enum Kind {
    DefinedKind,
    UndefinedKind,
    DylibKind,
    LazyKind,
  };

  Kind kind() const { return static_cast<Kind>(SymbolKind); }

  StringRef getName() const { return {Name.Data, Name.Size}; }

  uint64_t getVA() const;

  InputFile *File;

  bool IsWeak = false;

protected:
  Symbol(Kind K, InputFile *File, StringRefZ Name)
    : File(File), SymbolKind(K), Name(Name) {}

  Kind SymbolKind;
  StringRefZ Name;
};

class Defined : public Symbol {
public:
  Defined(StringRefZ Name, InputSection *IS, uint32_t Value)
    : Symbol(DefinedKind, nullptr, Name), IS(IS), Value(Value) {}

  InputSection *IS;
  uint32_t Value;

  static bool classof(const Symbol *S) { return S->kind() == DefinedKind; }
};

class Undefined : public Symbol {
public:
  Undefined(StringRefZ Name) : Symbol(UndefinedKind, nullptr, Name) {}

  static bool classof(const Symbol *S) { return S->kind() == UndefinedKind; }
};

class DylibSymbol : public Symbol {
public:
  DylibSymbol(InputFile *File, StringRefZ Name)
    : Symbol(DylibKind, File, Name) {}
  static bool classof(const Symbol *S) { return S->kind() == DylibKind; }
};

class LazySymbol : public Symbol {
public:
  LazySymbol(InputFile *File, const llvm::object::Archive::Symbol Sym)
    : Symbol(LazyKind, File, Sym.getName()), Sym(Sym) {}

  static bool classof(const Symbol *S) { return S->kind() == LazyKind; }

  InputFile *fetch();

private:
  const llvm::object::Archive::Symbol Sym;
};

inline uint64_t Symbol::getVA() const {
  if (auto *D = dyn_cast<Defined>(this))
    return D->IS->Addr + D->Value - ImageBase;
  return 0;
}

union SymbolUnion {
  alignas(Defined) char A[sizeof(Defined)];
  alignas(Undefined) char B[sizeof(Undefined)];
  alignas(DylibSymbol) char C[sizeof(DylibSymbol)];
  alignas(LazySymbol) char D[sizeof(LazySymbol)];
};

template <typename T, typename... ArgT>
void replaceSymbol(Symbol *S, ArgT &&... Arg) {
  static_assert(sizeof(T) <= sizeof(SymbolUnion), "SymbolUnion too small");
  static_assert(alignof(T) <= alignof(SymbolUnion),
                "SymbolUnion not aligned enough");
  assert(static_cast<Symbol *>(static_cast<T *>(nullptr)) == nullptr &&
         "Not a Symbol");

  new (S) T(std::forward<ArgT>(Arg)...);
}

} // namespace macho

std::string toString(const macho::Symbol &);
} // namespace lld

#endif
