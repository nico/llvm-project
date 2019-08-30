#include "SymbolTable.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"

using namespace llvm;
using namespace lld;
using namespace macho;

Symbol *SymbolTable::find(StringRef Name) {
  auto It = SymMap.find(llvm::CachedHashStringRef(Name));
  if (It == SymMap.end())
    return nullptr;
  return SymVector[It->second];
}

std::pair<Symbol *, bool> SymbolTable::insert(StringRef Name) {
  auto P = SymMap.insert({CachedHashStringRef(Name), (int)SymVector.size()});

  // Name already present in the symbol table.
  if (!P.second)
    return {SymVector[P.first->second], false};

  // Name is a new symbol.
  Symbol *Sym = (Symbol *)make<SymbolUnion>();
  SymVector.push_back(Sym);
  return {Sym, true};
}

// We have a new defined symbol with the specified binding. Return 1 if the new
// symbol should win, -1 if the new symbol should lose, or 0 if both symbols are
// strong defined symbols.
static int compareDefined(Symbol *S, bool WasInserted, bool IsWeak,
                          StringRef Name) {
  if (WasInserted)
    return 1;
  if (!isa<Defined>(S))
    return 1;
  if (IsWeak)
    return -1;
  if (S->IsWeak)
    return 1;
  return 0;
}

Symbol *SymbolTable::addDefined(StringRef Name, InputSection *IS,
                                uint32_t Value, bool IsWeak) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);
  int Cmp = compareDefined(S, WasInserted, IsWeak, Name);
  if (Cmp > 0) {
    replaceSymbol<Defined>(S, Name, IS, Value);
    S->IsWeak = IsWeak;
  }
  else if (Cmp == 0)
    error("duplicate symbol: " + Name);

  return S;
}

Symbol *SymbolTable::addUndefined(StringRef Name) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);

  if (WasInserted)
    replaceSymbol<Undefined>(S, Name);
  return S;
}

Symbol *SymbolTable::addDylib(StringRef Name, InputFile *File) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);

  if (WasInserted)
    replaceSymbol<DylibSymbol>(S, File, Name);
  return S;
}

Symbol *SymbolTable::addLazy(StringRef Name, ArchiveFile *File,
                             const llvm::object::Archive::Symbol Sym) {
  Symbol *S;
  bool WasInserted;
  std::tie(S, WasInserted) = insert(Name);

  if (WasInserted)
    replaceSymbol<LazySymbol>(S, File, Sym);
  else if (isa<Undefined>(S))
    File->fetch(Sym);
  return S;
}

SymbolTable *macho::Symtab;
