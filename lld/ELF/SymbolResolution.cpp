//===- SymbolResolution.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Symbol resolution: adding the symbols of the input files to the symbol
// table and extracting the lazy files (archive members, --start-lib objects)
// the link needs.
//
// The result is defined by parsing the files one at a time: file order
// decides which of several definitions wins, an undefined symbol extracts the
// lazy file that first offered a definition, and an extracted file is parsed
// right then, before the rest of the extracting file, so its definitions win
// over later ones. Everything observable follows from that order: the kept
// copies of weak and COMDAT definitions, the order of ctx.objectFiles (and
// so of the output's sections), the order of the symbol table (.symtab, the
// GOT and PLT), diagnostics. This file computes exactly that result, mostly
// on all threads.
//
// Every symbol table operation of a file is an EVENT with a KEY that is its
// position in the one-file-at-a-time execution. A file is a RECORD of events
// (a lazy file has a record for its lazy definitions and, once extracted,
// another for its full parse). An event that extracts a file makes the
// extracted file's record a CHILD of the event: its events come right after
// the extracting event. The tree of records is the extraction tree.
//
// The work is split in three:
//  1. A light pass over the ROOT records (the files given to the batch), in
//     parallel per shard of the symbol table (names hash to shards): look up
//     or create each event's symbol, and note per symbol the few facts that
//     decide extractions: the first lazy file that defines it, the first
//     definition, the first strong reference.
//  2. The extraction tree, by a depth-first walk in key order, on one
//     thread: a strong reference extracts the symbol's lazy definer if no
//     definition precedes it. That reads a few words per reference; the
//     real Symbol::resolve state machine is only run, over a symbol's
//     recorded events, for the rare symbols whose state depends on more
//     than that (defined by a DSO and referenced with non-default
//     visibility, common, from bitcode, versioned, traced, ...).
//  3. With the tree known, every event has its key: replay all events with
//     Symbol::resolve, in key order, in parallel per shard. Extractions
//     that resolve() asks for are checked against the tree.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include <mutex>
#include <optional>
#include <queue>

using namespace llvm;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {

// The position of an event in its file. Object files insert all their symbol
// names first (phase 0), then resolve the definitions (1), then the
// references (2); each reference may extract a file.
enum Phase : uint32_t { InsertPhase = 0, DefinePhase = 1, ReferPhase = 2 };
constexpr uint32_t phaseShift = 30;
static uint32_t makePos(Phase phase, uint32_t idx) {
  return (uint32_t(phase) << phaseShift) | idx;
}
static uint32_t posIdx(uint32_t pos) { return pos & ((1u << phaseShift) - 1); }

// A key for the replay: the depth-first ordinal of the event's segment (see
// Record) in the high half, the event's position in its file in the low
// half.
using Key = uint64_t;
static Key makeKey(uint32_t segOrd, uint32_t pos) {
  return (Key(segOrd) << 32) | pos;
}

// A key for building the tree, when the segments are not numbered yet: the
// root record the event is under, the position in that root of the event
// itself (for a root's event) or of the extraction that leads to it, and a
// timestamp of the walk (0 for a root's event, so that it precedes the
// subtree its position extracts).
struct LKey {
  uint32_t root, pos, ts;
  bool operator<(const LKey &o) const {
    return std::tie(root, pos, ts) < std::tie(o.root, o.pos, o.ts);
  }
  bool operator==(const LKey &o) const {
    return root == o.root && pos == o.pos && ts == o.ts;
  }
};
constexpr LKey beforeAll{0, 0, 0};
constexpr LKey never{UINT32_MAX, UINT32_MAX, UINT32_MAX};

// A file's symbol events: the lazy definitions of a lazy file, or its full
// parse.
struct Record {
  Record(InputFile *file, bool lazy) : file(file), lazy(lazy) {}
  InputFile *file;
  bool lazy;
  // A root that adds nothing: not compatible with the target (reported in
  // file order), or a DSO whose soname was seen before (traced).
  bool dead = false;
  bool incompatible = false;
  bool duplicateSoName = false;
  uint32_t parent = UINT32_MAX;
  uint32_t posInParent = 0;
  // For LKeys: the root this record is under, and the position in it.
  uint32_t root = 0, rootPos = 0;
  // The records of the files this record's events extract, in event order,
  // and the positions of those events.
  SmallVector<uint32_t, 0> children;
  SmallVector<uint32_t, 0> childPos;
  // children.size() + 1 segment ordinals, and the last ordinal in the
  // subtree, assigned by Resolver::renumber().
  SmallVector<uint32_t, 0> segOrd;
  uint32_t subtreeEnd = 0;
  // For a lazy record: the lazy definition that extracted the file itself
  // (an undefined symbol was waiting for it). Lazy definitions after it do
  // not happen, as the loop that adds them stopped there.
  uint32_t lazyCutoff = UINT32_MAX;

  // The segment an event at pos falls in: after the children extracted by
  // earlier events.
  size_t segmentOf(uint32_t pos) const {
    return llvm::lower_bound(childPos, pos) - childPos.begin();
  }
  Key keyOf(uint32_t pos) const { return makeKey(segOrd[segmentOf(pos)], pos); }
};

// An entry in a symbol's event chain: what the exact replay of a complex
// symbol needs.
struct Node {
  uint32_t rec, event, next;
  LKey key;
};

// What is known about a symbol for building the tree.
struct SymInfo {
  // The lazy file whose definition the symbol refers to while lazy: the
  // first lazy definition; and all lazy definers, if two suffice.
  InputFile *owner = nullptr;
  uint32_t ownerRec = 0, ownerEvent = 0;
  LKey ownerKey = never;
  InputFile *definer[2] = {nullptr, nullptr};
  bool moreDefiners = false;
  // The first definition (regular, common or shared) and the first strong
  // reference among the roots' events.
  LKey firstDef = never;
  LKey firstStrongRef = never;
  // The walk found a definition of it in an extracted file.
  bool definedInWave = false;
  // Asked for the extraction of its owner at ownerKey already.
  bool ownerRequested = false;
  // The simple rules do not apply; the exact replay of the chain does.
  bool complex = false;
  bool sharedDef = false, hiddenRef = false;
  // A symbol from before the batch has been looked at.
  bool classified = false;
  uint32_t head = UINT32_MAX;
  // For the replay: the key of the first insert, for the symbol table's
  // order; the --warn-backrefs entry.
  Key firstKey = UINT64_MAX;
  const InputFile *backrefFrom = nullptr, *backrefTo = nullptr;
  bool hasBackref = false;
};

// An extraction request while building the tree.
struct Extraction {
  LKey key;
  InputFile *file;
  uint32_t rec, pos;
  bool operator>(const Extraction &o) const { return o.key < key; }
};

struct Diag {
  uint32_t rec, pos;
  DiagLevel level;
  std::string msg;
};

struct WhyExtract {
  const InputFile *reference;
  const InputFile *extracted;
  const Symbol *sym;
};

struct BatchShard {
  std::vector<Node> nodes;
  std::vector<SymInfo> info;
  std::vector<Diag> diags;
  std::vector<WhyExtract> whyExtract;
  // shard.syms.size() when the batch started: later slots are new symbols.
  uint32_t base = 0;
  // The light pass found an entry whose symbol is kept in another shard.
  bool foreign = false;
};

// What Symbol::extract() and the diagnostics need to know about the event
// being applied on this thread.
struct ReplayContext {
  enum Mode { Scratch, Replay } mode;
  BatchShard *shard;
  uint32_t slot;
  uint32_t rec, pos;
  // What extract() was asked for by the current event.
  InputFile *requested = nullptr;
  const InputFile *reference = nullptr;
  bool backref = false;
};

thread_local ReplayContext *currentContext = nullptr;

template <class ELFT> class Resolver {
public:
  Resolver(Ctx &ctx, bool ltoObjects)
      : ctx(ctx), symtab(*ctx.symtab), ltoObjects(ltoObjects),
        shards(SymbolTable::numShards) {}
  void run(ArrayRef<InputFile *> files);

private:
  void prepare(ArrayRef<uint32_t> recs);
  void lightPass();
  void lightEvent(unsigned s, uint32_t r, uint32_t e, bool ref, bool follow,
                  bool serial);
  void classify(SymInfo &d, const Symbol &sym);
  void rootRequests();
  bool decideComplex(SymInfo &d, unsigned s, uint32_t slot, uint32_t r,
                     uint32_t e, LKey key, InputFile *&file, bool refIsLast,
                     std::vector<Extraction> &requests);
  bool definerExtracted(const SymInfo &d, unsigned s);
  void buildTree();
  void extract(InputFile *file, uint32_t parentRec, uint32_t pos, uint32_t &ts);
  void renumber();
  void renumber(uint32_t rec, uint32_t &ord);
  void replay();
  void walk(unsigned s, uint32_t r);
  void walkAll(uint32_t r);
  void apply(uint32_t r, uint32_t e, uint32_t pos, bool lazy,
             ReplayContext &rc);
  void finishExtraction(ReplayContext &rc, uint32_t pos);
  void applyInsert(Record &rec, uint32_t e, Symbol *sym);
  void applyResolve(Record &rec, uint32_t e, Symbol *sym);
  void finish();

  bool eventIsReference(Record &rec, uint32_t e);
  CachedHashStringRef eventName(Record &rec, uint32_t e, StringRef &name);
  // Where event e of rec is in the record: object and bitcode files resolve
  // their definitions before their references, the others their events in
  // order.
  uint32_t eventPos(Record &rec, uint32_t e, bool ref) {
    bool split = !rec.lazy && (rec.file->kind() == InputFile::ObjKind ||
                               rec.file->kind() == InputFile::BitcodeKind);
    return makePos(split && ref ? ReferPhase : DefinePhase, e);
  }
  // The symbol of event e of rec; a symbol from before the batch that no
  // root of the batch mentions is classified on first contact.
  SymInfo &infoOf(const Record &rec, uint32_t e, unsigned &s, uint32_t &slot) {
    uint32_t home = rec.file->symbolEvents.homes[e];
    s = home >> SymbolTable::slotBits;
    slot = home & ((1u << SymbolTable::slotBits) - 1);
    BatchShard &shard = shards[s];
    if (slot >= shard.info.size())
      shard.info.resize(slot + 1);
    SymInfo &d = shard.info[slot];
    if (slot < shard.base && !d.classified)
      classify(d, *symtab.shard(s).syms[slot]);
    return d;
  }

  Ctx &ctx;
  SymbolTable &symtab;
  // The batch is the output of LTO: not traced, COMDAT groups not selected.
  bool ltoObjects;
  std::vector<Record> records;
  SmallVector<uint32_t, 0> roots;
  // The parse record of each extracted file.
  DenseMap<InputFile *, uint32_t> parseRecord;
  std::vector<BatchShard> shards;
  // The extraction requests of the walk, in key order.
  std::priority_queue<Extraction, std::vector<Extraction>,
                      std::greater<Extraction>>
      pending;
  // Some symbols are reachable from two shards (after --wrap): replay on
  // one thread.
  bool serialReplay = false;
  // The COMDAT scan running on the pool during the tree walk.
  std::optional<llvm::parallel::TaskGroup> scanGroup;
};

// --- Per-file event interfaces ---------------------------------------------

template <class ELFT>
bool Resolver<ELFT>::eventIsReference(Record &rec, uint32_t e) {
  switch (rec.file->kind()) {
  case InputFile::ObjKind:
    return cast<ObjFile<ELFT>>(rec.file)->isReferenceEvent(e);
  case InputFile::SharedKind:
    return cast<SharedFile>(rec.file)->isReferenceEvent(e);
  case InputFile::BitcodeKind:
    return cast<BitcodeFile>(rec.file)->isReferenceEvent(e);
  default:
    return false;
  }
}

template <class ELFT>
CachedHashStringRef Resolver<ELFT>::eventName(Record &rec, uint32_t e,
                                              StringRef &name) {
  switch (rec.file->kind()) {
  case InputFile::ObjKind:
    return cast<ObjFile<ELFT>>(rec.file)->eventName(e, name);
  case InputFile::SharedKind:
    return cast<SharedFile>(rec.file)->eventName(e, name);
  case InputFile::BitcodeKind:
    return cast<BitcodeFile>(rec.file)->eventName(e, name);
  case InputFile::BinaryKind:
    return cast<BinaryFile>(rec.file)->eventName(e, name);
  default:
    llvm_unreachable("file without symbol events");
  }
}

template <class ELFT>
void Resolver<ELFT>::applyInsert(Record &rec, uint32_t e, Symbol *sym) {
  StringRef name;
  CachedHashStringRef stem = eventName(rec, e, name);
  symtab.initOrRename(sym, !sym->getName().data(), stem, name);
}

template <class ELFT>
void Resolver<ELFT>::applyResolve(Record &rec, uint32_t e, Symbol *sym) {
  switch (rec.file->kind()) {
  case InputFile::ObjKind:
    cast<ObjFile<ELFT>>(rec.file)->applyEvent(e, sym, rec.lazy);
    break;
  case InputFile::SharedKind:
    cast<SharedFile>(rec.file)->applyEvent<ELFT>(e, sym);
    break;
  case InputFile::BitcodeKind:
    cast<BitcodeFile>(rec.file)->applyEvent(e, sym, rec.lazy);
    break;
  case InputFile::BinaryKind:
    cast<BinaryFile>(rec.file)->applyEvent(e, sym);
    break;
  default:
    llvm_unreachable("file without symbol events");
  }
}

// --- Preparing files ---------------------------------------------------------

// The per-file half: everything the events need that does not depend on the
// symbol table, in parallel.
template <class ELFT> void Resolver<ELFT>::prepare(ArrayRef<uint32_t> recs) {
  parallelForEach(recs, [&](uint32_t r) {
    Record &rec = records[r];
    InputFile *file = rec.file;
    if (rec.dead || file->symbolEvents.prepared)
      return;
    file->symbolEvents.prepared = true;
    switch (file->kind()) {
    case InputFile::ObjKind:
      cast<ObjFile<ELFT>>(file)->prepareSymbolEvents();
      break;
    case InputFile::SharedKind:
      cast<SharedFile>(file)->prepareSymbolEvents<ELFT>();
      break;
    case InputFile::BitcodeKind:
      cast<BitcodeFile>(file)->prepareSymbolEvents();
      break;
    case InputFile::BinaryKind:
      return; // Below, in order: it allocates with make<>.
    default:
      llvm_unreachable("file without symbol events");
    }
    InputFile::SymbolEvents &ev = file->symbolEvents;
    ev.homes = std::make_unique<uint32_t[]>(ev.num);
    ev.bits = std::make_unique<uint8_t[]>(ev.num);
    switch (file->kind()) {
    case InputFile::ObjKind: {
      auto *f = cast<ObjFile<ELFT>>(file);
      for (uint32_t e = 0; e < ev.num; ++e) {
        const typename ELFT::Sym &eSym = f->eventSym(e);
        uint8_t b = 0;
        if (eSym.st_shndx == SHN_UNDEF)
          b |= InputFile::SymbolEvents::Ref;
        else if (eSym.st_shndx == SHN_COMMON)
          b |= InputFile::SymbolEvents::Common;
        if (eSym.getBinding() == STB_WEAK)
          b |= InputFile::SymbolEvents::Weak;
        if (f->eventNameHasAt(e))
          b |= InputFile::SymbolEvents::HasAt;
        if ((eSym.st_other & 3) != STV_DEFAULT)
          b |= InputFile::SymbolEvents::NonDefaultVis;
        ev.bits[e] = b;
      }
      break;
    }
    case InputFile::SharedKind: {
      auto *f = cast<SharedFile>(file);
      for (uint32_t e = 0; e < ev.num; ++e) {
        uint8_t b = 0;
        if (f->isReferenceEvent(e)) {
          b |= InputFile::SymbolEvents::Ref;
          if (f->isWeakReference(e))
            b |= InputFile::SymbolEvents::Weak;
        }
        if (f->eventNameHasAt(e))
          b |= InputFile::SymbolEvents::HasAt;
        ev.bits[e] = b;
      }
      break;
    }
    default:
      for (uint32_t e = 0; e < ev.num; ++e) {
        uint8_t b = InputFile::SymbolEvents::Other;
        if (auto *f = dyn_cast<BitcodeFile>(file))
          if (f->isReferenceEvent(e))
            b |= InputFile::SymbolEvents::Ref;
        ev.bits[e] = b;
      }
      break;
    }
  });
  // What has to happen in file order, or is not thread-safe.
  for (uint32_t r : recs) {
    Record &rec = records[r];
    if (rec.dead)
      continue;
    if (auto *f = dyn_cast<SharedFile>(rec.file)) {
      // DSOs are added once per soname.
      if (!f->registerSoName())
        rec.dead = rec.duplicateSoName = true;
    } else if (auto *f = dyn_cast<BinaryFile>(rec.file)) {
      f->prepareSymbolEvents();
      f->symbolEvents.homes = std::make_unique<uint32_t[]>(f->symbolEvents.num);
      f->symbolEvents.bits = std::make_unique<uint8_t[]>(f->symbolEvents.num);
      std::fill_n(f->symbolEvents.bits.get(), f->symbolEvents.num,
                  InputFile::SymbolEvents::Other);
    }
  }
}

// --- The light pass ----------------------------------------------------------

// What a symbol from before the batch is, for the tree.
template <class ELFT>
void Resolver<ELFT>::classify(SymInfo &d, const Symbol &sym) {
  d.classified = true;
  if (sym.traced)
    d.complex = true;
  if (sym.isPlaceholder())
    return;
  if (sym.isDefined() || sym.isCommon() || sym.isShared()) {
    d.firstDef = beforeAll;
    d.sharedDef |= sym.isShared();
    d.complex |= sym.isCommon();
  } else if (sym.isUndefined()) {
    if (!sym.isWeak())
      d.firstStrongRef = beforeAll;
  } else {
    d.complex = true; // Lazy from an earlier batch.
  }
}

// Looks up (creating it if needed) the symbol of event e of root record r
// and, unless the event does not happen (a lazy file's reference: follow is
// false), notes what it tells about the symbol. In the parallel pass, shard
// s handles the names that hash to it; a symbol kept in another shard (a
// name --wrap redirected) is left for the serial pass.
template <class ELFT>
void Resolver<ELFT>::lightEvent(unsigned s, uint32_t r, uint32_t e, bool ref,
                                bool follow, bool serial) {
  Record &rec = records[r];
  InputFile *file = rec.file;
  StringRef name;
  CachedHashStringRef stem = eventName(rec, e, name);
  bool isNew;
  SymbolTable::Entry &entry = symtab.lookup(stem, isNew);
  if (isNew) {
    entry.sym = reinterpret_cast<Symbol *>(makeThreadLocal<SymbolUnion>());
    entry.home = symtab.addSlot(s, entry.sym);
  }
  file->symbolEvents.homes[e] = entry.home;
  unsigned home = entry.home >> SymbolTable::slotBits;
  uint32_t slot = entry.home & ((1u << SymbolTable::slotBits) - 1);
  if (home != s && !serial) {
    shards[s].foreign = true;
    return;
  }
  BatchShard &shard = shards[home];
  if (slot >= shard.info.size())
    shard.info.resize(slot + 1);
  SymInfo &d = shard.info[slot];
  if (slot < shard.base && !d.classified)
    classify(d, *entry.sym);
  if (!follow)
    return;
  uint32_t pos = eventPos(rec, e, ref);
  LKey key{rec.root, pos, 0};
  shard.nodes.push_back({r, e, d.head, key});
  d.head = shard.nodes.size() - 1;
  if (stem.size() != name.size())
    d.complex = true;
  uint8_t bits = rec.file->symbolEvents.bits[e];
  if (bits & InputFile::SymbolEvents::Other) {
    d.complex = true;
    return;
  }
  if (rec.lazy) {
    if (!d.owner) {
      d.owner = file;
      d.ownerRec = r;
      d.ownerEvent = e;
      d.ownerKey = key;
    }
    if (!d.definer[0] || d.definer[0] == file)
      d.definer[0] = file;
    else if (!d.definer[1] || d.definer[1] == file)
      d.definer[1] = file;
    else
      d.moreDefiners = true;
  } else if (ref) {
    if (!(bits & InputFile::SymbolEvents::Weak))
      d.firstStrongRef = std::min(d.firstStrongRef, key);
    if (bits & InputFile::SymbolEvents::NonDefaultVis)
      d.hiddenRef = true;
  } else {
    d.firstDef = std::min(d.firstDef, key);
    if (bits & InputFile::SymbolEvents::Common)
      d.complex = true;
    if (file->kind() == InputFile::SharedKind)
      d.sharedDef = true;
  }
}

template <class ELFT> void Resolver<ELFT>::lightPass() {
  auto pass = [&](unsigned s, bool serial) {
    for (uint32_t r : roots) {
      Record &rec = records[r];
      if (rec.dead)
        continue;
      // A lazy file's references only happen if it is extracted; they are
      // looked up now so that the walk does not have to.
      const InputFile::SymbolEvents &ev = rec.file->symbolEvents;
      for (ArrayRef<uint32_t> events : {ev.definitions(s), ev.references(s)}) {
        for (uint32_t e : events) {
          bool ref = ev.bits[e] & InputFile::SymbolEvents::Ref;
          lightEvent(s, r, e, ref, !(rec.lazy && ref), serial);
        }
      }
    }
  };
  parallelFor(0, SymbolTable::numShards,
              [&](size_t s) { pass(s, /*serial=*/false); });
  for (BatchShard &shard : shards)
    serialReplay |= shard.foreign;
  if (!serialReplay)
    return;
  // Start over on one thread, each event going to its symbol's shard.
  for (unsigned s = 0; s < SymbolTable::numShards; ++s) {
    BatchShard &shard = shards[s];
    shard.nodes.clear();
    shard.info.clear();
    shard.info.resize(shard.base);
  }
  for (unsigned s = 0; s < SymbolTable::numShards; ++s)
    pass(s, /*serial=*/true);
}

// --- Building the tree -------------------------------------------------------

// The exact rule for a complex symbol: replay its chain, on the symbol
// itself (restored afterwards), with the real state machine. If refIsLast,
// event e of record r (at key) is the reference being decided: returns
// whether it extracts a file and which; otherwise every extraction the
// replay makes after key becomes a request (the roots' lazy definitions).
template <class ELFT>
bool Resolver<ELFT>::decideComplex(SymInfo &d, unsigned s, uint32_t slot,
                                   uint32_t r, uint32_t e, LKey key,
                                   InputFile *&file, bool refIsLast,
                                   std::vector<Extraction> &requests) {
  BatchShard &shard = shards[s];
  Symbol *sym = symtab.shard(s).syms[slot];
  SmallVector<std::pair<LKey, uint32_t>, 32> nodes;
  for (uint32_t n = d.head; n != UINT32_MAX; n = shard.nodes[n].next)
    nodes.push_back({shard.nodes[n].key, n});
  llvm::sort(nodes, [](auto &a, auto &b) { return a.first < b.first; });
  SymbolUnion saved;
  memcpy(&saved, sym, sizeof(SymbolUnion));
  if (slot >= shard.base)
    memset(static_cast<void *>(sym), 0, sizeof(SymbolUnion));
  ReplayContext rc{ReplayContext::Scratch, &shard, slot, 0, 0};
  currentContext = &rc;
  bool extracts = false;
  for (auto [k, n] : nodes) {
    const Node &node = shard.nodes[n];
    Record &rec = records[node.rec];
    if (rec.dead || (rec.lazy && node.event > rec.lazyCutoff))
      continue;
    if (refIsLast && key < k)
      break;
    rc.rec = node.rec;
    rc.requested = nullptr;
    applyInsert(rec, node.event, sym);
    applyResolve(rec, node.event, sym);
    if (!rc.requested)
      continue;
    if (refIsLast) {
      if (k == key && node.rec == r && node.event == e) {
        file = rc.requested;
        extracts = true;
      }
    } else if (key < k && rc.requested->lazy) {
      requests.push_back(
          {k, rc.requested, node.rec,
           eventPos(rec, node.event,
                    !rec.lazy && eventIsReference(rec, node.event))});
    }
  }
  currentContext = nullptr;
  memcpy(static_cast<void *>(sym), &saved, sizeof(SymbolUnion));
  return extracts;
}

// Whether a lazy definer of the symbol has been extracted (so that the
// symbol is defined and no reference extracts another).
template <class ELFT>
bool Resolver<ELFT>::definerExtracted(const SymInfo &d, unsigned s) {
  if (!d.moreDefiners)
    return (d.definer[0] && !d.definer[0]->lazy) ||
           (d.definer[1] && !d.definer[1]->lazy);
  for (uint32_t n = d.head; n != UINT32_MAX; n = shards[s].nodes[n].next) {
    const Record &rec = records[shards[s].nodes[n].rec];
    if (rec.lazy && !rec.file->lazy)
      return true;
  }
  return false;
}

// The roots' extractions: from the simple facts for simple symbols, from the
// exact replay for complex ones.
template <class ELFT> void Resolver<ELFT>::rootRequests() {
  std::mutex mu;
  parallelFor(0, SymbolTable::numShards, [&](size_t s) {
    BatchShard &shard = shards[s];
    std::vector<Extraction> local;
    for (uint32_t slot = 0; slot < shard.info.size(); ++slot) {
      SymInfo &d = shard.info[slot];
      if (d.head == UINT32_MAX)
        continue;
      if ((d.sharedDef && d.hiddenRef) ||
          (ctx.arg.fortranCommon && d.definer[1]))
        d.complex = true;
      if (d.complex) {
        InputFile *file = nullptr;
        decideComplex(d, s, slot, 0, 0, beforeAll, file, false, local);
        continue;
      }
      // A strong reference before the lazy definition extracts the file at
      // the lazy definition; the first one after it, there. In both cases
      // only if no definition comes before.
      if (!d.owner || d.firstStrongRef == never)
        continue;
      LKey ref = d.firstStrongRef;
      LKey at = ref < d.ownerKey ? d.ownerKey : ref;
      if (d.firstDef < at)
        continue;
      if (ref < d.ownerKey) {
        local.push_back(
            {at, d.owner, d.ownerRec, makePos(DefinePhase, d.ownerEvent)});
        d.ownerRequested = true;
      } else {
        local.push_back({at, d.owner, roots[ref.root], ref.pos});
      }
    }
    std::lock_guard<std::mutex> lock(mu);
    for (Extraction &x : local)
      pending.push(x);
  });
}

// Extracts file at position pos of record parentRec, and walks its events:
// its definitions define their symbols; a strong reference to a symbol that
// is lazy at that point extracts the symbol's owner, right there.
template <class ELFT>
void Resolver<ELFT>::extract(InputFile *file, uint32_t parentRec, uint32_t pos,
                             uint32_t &ts) {
  uint32_t r = records.size();
  records.emplace_back(file, /*lazy=*/false);
  parseRecord[file] = r;
  file->lazy = false;
  {
    Record &child = records[r];
    Record &parent = records[parentRec];
    child.parent = parentRec;
    child.posInParent = pos;
    child.root = parent.root;
    child.rootPos = parent.parent == UINT32_MAX ? pos : parent.rootPos;
    parent.children.push_back(r);
    parent.childPos.push_back(pos);
    if (parent.lazy)
      parent.lazyCutoff = posIdx(pos);
  }
  if (!file->symbolEvents.prepared)
    prepare({r});
  const InputFile::SymbolEvents &ev = file->symbolEvents;
  auto note = [&](uint32_t e, unsigned s, SymInfo &d) {
    LKey key{records[r].root, records[r].rootPos, ++ts};
    shards[s].nodes.push_back({r, e, d.head, key});
    d.head = shards[s].nodes.size() - 1;
    return key;
  };

  // Definitions first, then references, as the file resolves them.
  for (uint32_t e = 0; e < ev.num; ++e) {
    uint8_t bits = ev.bits[e];
    if (bits & InputFile::SymbolEvents::Ref)
      continue;
    unsigned s;
    uint32_t slot;
    SymInfo &d = infoOf(records[r], e, s, slot);
    d.definedInWave = true;
    LKey key = note(e, s, d);
    if (bits & (InputFile::SymbolEvents::Other | InputFile::SymbolEvents::Common |
                InputFile::SymbolEvents::HasAt))
      d.complex = true;
    if (d.complex) {
      // The roots' events after this definition may extract differently
      // now (a lazy definition over a common symbol, say).
      std::vector<Extraction> requests;
      InputFile *unused = nullptr;
      decideComplex(d, s, slot, r, e, key, unused, false, requests);
      for (Extraction &x : requests)
        pending.push(x);
    }
  }
  for (uint32_t e = 0; e < ev.num; ++e) {
    uint8_t bits = ev.bits[e];
    if (!(bits & InputFile::SymbolEvents::Ref))
      continue;
    unsigned s;
    uint32_t slot;
    SymInfo &d = infoOf(records[r], e, s, slot);
    uint32_t refPos = eventPos(records[r], e, true);
    LKey key = note(e, s, d);
    bool weak = bits & InputFile::SymbolEvents::Weak;
    if (bits & InputFile::SymbolEvents::NonDefaultVis) {
      d.hiddenRef = true;
      if (d.sharedDef)
        d.complex = true;
    }
    if (bits & InputFile::SymbolEvents::HasAt)
      d.complex = true;
    if (bits & InputFile::SymbolEvents::Other) {
      d.complex = true;
      weak = false;
    }
    if (d.complex) {
      std::vector<Extraction> none;
      InputFile *target = nullptr;
      if (decideComplex(d, s, slot, r, e, key, target, true, none) &&
          target->lazy)
        extract(target, r, refPos, ts);
      continue;
    }
    if (weak || !d.owner || d.definedInWave || d.firstDef < key)
      continue;
    if (d.ownerKey < key) {
      // Lazy here, unless a definer of the symbol was extracted already.
      if (!definerExtracted(d, s))
        extract(d.owner, r, refPos, ts);
    } else if (!d.ownerRequested && !(d.firstDef < d.ownerKey)) {
      // Strongly referenced before its lazy definition: that will extract
      // the file.
      d.ownerRequested = true;
      pending.push({d.ownerKey, d.owner, d.ownerRec,
                    makePos(DefinePhase, d.ownerEvent)});
    }
  }
}

template <class ELFT> void Resolver<ELFT>::buildTree() {
  uint32_t ts = 0;
  while (!pending.empty()) {
    Extraction x = pending.top();
    pending.pop();
    if (!x.file->lazy)
      continue;
    // Still due? A definition of the symbol (the requesting event's) may
    // have come before.
    Record &rec = records[x.rec];
    uint32_t e = posIdx(x.pos);
    unsigned s;
    uint32_t slot;
    SymInfo &d = infoOf(rec, e, s, slot);
    if (d.complex) {
      std::vector<Extraction> none;
      InputFile *target = nullptr;
      if (!decideComplex(d, s, slot, x.rec, e, x.key, target, true, none) ||
          target != x.file)
        continue;
    } else if (d.definedInWave || d.firstDef < x.key ||
               definerExtracted(d, s)) {
      continue;
    }
    extract(x.file, x.rec, x.pos, ts);
  }
}

// --- Numbering ---------------------------------------------------------------

template <class ELFT> void Resolver<ELFT>::renumber(uint32_t r, uint32_t &ord) {
  Record &rec = records[r];
  rec.segOrd.resize(rec.children.size() + 1);
  rec.segOrd[0] = ord++;
  for (size_t i = 0; i < rec.children.size(); ++i) {
    renumber(rec.children[i], ord);
    rec.segOrd[i + 1] = ord++;
  }
  rec.subtreeEnd = ord - 1;
}

template <class ELFT> void Resolver<ELFT>::renumber() {
  uint32_t ord = 0;
  for (uint32_t r : roots)
    renumber(r, ord);
}

// --- The replay --------------------------------------------------------------

template <class ELFT>
void Resolver<ELFT>::apply(uint32_t r, uint32_t e, uint32_t pos, bool lazy,
                           ReplayContext &rc) {
  Record &rec = records[r];
  unsigned s;
  uint32_t slot;
  SymInfo &d = infoOf(rec, e, s, slot);
  Symbol *sym = symtab.shard(s).syms[slot];
  rc.shard = &shards[s];
  rc.slot = slot;
  rc.rec = r;
  rc.pos = pos;
  rc.requested = nullptr;
  if (slot >= shards[s].base) {
    Key insertKey = !lazy && rec.file->kind() == InputFile::ObjKind
                        ? makeKey(rec.segOrd[0], makePos(InsertPhase, e))
                        : rec.keyOf(pos);
    d.firstKey = std::min(d.firstKey, insertKey);
  }
  currentContext = &rc;
  applyInsert(rec, e, sym);
  applyResolve(rec, e, sym);
  currentContext = nullptr;
}

// Once the events of the file extracted at pos are in: the --why-extract
// record and the --warn-backrefs check, on the symbol as the extraction
// left it.
template <class ELFT>
void Resolver<ELFT>::finishExtraction(ReplayContext &rc, uint32_t pos) {
  assert(rc.requested && rc.pos == pos && "the walk missed an extraction");
  SymInfo &d = rc.shard->info[rc.slot];
  Symbol *sym = symtab.shard(rc.shard - shards.data()).syms[rc.slot];
  if (rc.reference && !ctx.arg.whyExtract.empty())
    rc.shard->whyExtract.push_back({rc.reference, sym->file, sym});
  if (rc.backref && !sym->isWeak() && !d.hasBackref) {
    d.hasBackref = true;
    d.backrefFrom = rc.reference;
    d.backrefTo = sym->file;
  }
  rc.requested = nullptr;
}

// Replays the events of record r that shard s owns, in key order: the
// definitions, then the references, with the extracted files' subtrees at
// their positions.
template <class ELFT> void Resolver<ELFT>::walk(unsigned s, uint32_t r) {
  Record &rec = records[r];
  if (rec.dead)
    return;
  const InputFile::SymbolEvents &ev = rec.file->symbolEvents;
  ReplayContext rc{ReplayContext::Replay, &shards[s], 0, r, 0};
  ReplayContext extracting = rc;
  size_t child = 0;
  // Enters the subtrees at positions up to and including upTo.
  auto enterChildren = [&](uint32_t upTo) {
    while (child < rec.children.size() && rec.childPos[child] <= upTo) {
      uint32_t pos = rec.childPos[child];
      walk(s, rec.children[child++]);
      if (extracting.requested && extracting.pos == pos)
        finishExtraction(extracting, pos);
    }
  };
  auto applyOne = [&](uint32_t e, uint32_t pos) {
    enterChildren(pos - 1);
    apply(r, e, pos, rec.lazy, rc);
    if (rc.requested)
      extracting = rc;
  };
  for (uint32_t e : ev.definitions(s))
    if (!(rec.lazy && e > rec.lazyCutoff))
      applyOne(e, makePos(DefinePhase, e));
  if (!rec.lazy)
    for (uint32_t e : ev.references(s))
      applyOne(e, makePos(ReferPhase, e));
  enterChildren(UINT32_MAX);
}

// One thread, all shards: the events in (phase, index) order.
template <class ELFT> void Resolver<ELFT>::walkAll(uint32_t r) {
  Record &rec = records[r];
  if (rec.dead)
    return;
  const InputFile::SymbolEvents &ev = rec.file->symbolEvents;
  SmallVector<std::pair<uint32_t, uint32_t>, 0> events; // (pos, e)
  for (unsigned s = 0; s < SymbolTable::numShards; ++s) {
    for (uint32_t e : ev.definitions(s))
      if (!(rec.lazy && e > rec.lazyCutoff))
        events.push_back({makePos(DefinePhase, e), e});
    if (!rec.lazy)
      for (uint32_t e : ev.references(s))
        events.push_back({makePos(ReferPhase, e), e});
  }
  llvm::sort(events);
  ReplayContext rc{ReplayContext::Replay, &shards[0], 0, r, 0};
  ReplayContext extracting = rc;
  size_t child = 0;
  auto enterChildren = [&](uint32_t upTo) {
    while (child < rec.children.size() && rec.childPos[child] <= upTo) {
      uint32_t pos = rec.childPos[child];
      walkAll(rec.children[child++]);
      if (extracting.requested && extracting.pos == pos)
        finishExtraction(extracting, pos);
    }
  };
  for (auto [pos, e] : events) {
    enterChildren(pos - 1);
    apply(r, e, pos, rec.lazy, rc);
    if (rc.requested)
      extracting = rc;
  }
  enterChildren(UINT32_MAX);
}

template <class ELFT> void Resolver<ELFT>::replay() {
  if (serialReplay) {
    for (uint32_t r : roots)
      walkAll(r);
    return;
  }
  parallelFor(0, SymbolTable::numShards, [&](size_t s) {
    for (uint32_t r : roots)
      walk(s, r);
  });
}

// --- Batch end ---------------------------------------------------------------

template <class ELFT> void Resolver<ELFT>::finish() {
  // The files in the one-file-at-a-time order (a depth-first walk). What
  // happened at each file's parse in that order: the file's registration,
  // its --trace line, the choice of its COMDAT groups (the first file with a
  // signature keeps it), dependent libraries, ARM attributes. The section
  // pre-pass that used to make those choices is now parallel: scan the
  // groups per file, choose them per shard of the signature hash in file
  // order, apply the choices per file, and do the rest in order.
  SmallVector<uint32_t, 0> order; // records, non-lazy, in the walk's order
  SmallVector<InputFile *, 0> parsed;
  {
    llvm::TimeTraceScope timeScope("Register files");
    SmallVector<uint32_t, 0> stack;
    for (uint32_t r : llvm::reverse(roots))
      stack.push_back(r);
    while (!stack.empty()) {
      uint32_t r = stack.pop_back_val();
      Record &rec = records[r];
      for (uint32_t c : llvm::reverse(rec.children))
        stack.push_back(c);
      if (rec.lazy && !rec.dead) {
        if (auto *f = dyn_cast<BitcodeFile>(rec.file))
          ctx.lazyBitcodeFiles.push_back(f);
        continue;
      }
      order.push_back(r);
      if (!rec.dead)
        parsed.push_back(rec.file);
    }

    {
      llvm::TimeTraceScope t("scan comdats");
      // Normally scanned by the pool during "Extract files"; wait for that.
      if (scanGroup)
        scanGroup.reset();
      else
        parallelForEach(parsed, [&](InputFile *file) {
          if (auto *f = dyn_cast<ObjFile<ELFT>>(file))
            f->scanComdats();
          else if (auto *f = dyn_cast<BitcodeFile>(file))
            f->scanComdats();
        });
    }
    if (!ltoObjects) {
      llvm::TimeTraceScope t("choose comdats");
      parallelFor(0, SymbolTable::numShards, [&](size_t s) {
        for (InputFile *file : parsed) {
          if (auto *f = dyn_cast<ObjFile<ELFT>>(file))
            f->chooseComdats(s);
          else if (auto *f = dyn_cast<BitcodeFile>(file))
            f->chooseComdats(s);
        }
      });
    }
    {
      llvm::TimeTraceScope t("parse sections");
      parallelForEach(parsed, [&](InputFile *file) {
        if (auto *f = dyn_cast<ObjFile<ELFT>>(file))
          f->parse(/*ignoreComdats=*/ltoObjects);
      });
    }
    llvm::TimeTraceScope t2("serial tail");
    for (uint32_t r : order) {
      Record &rec = records[r];
      InputFile *file = rec.file;
      if (rec.dead) {
        if (rec.incompatible)
          reportIncompatible(ctx, file);
        else if (rec.duplicateSoName && ctx.arg.trace)
          Msg(ctx) << file;
        continue;
      }
      if (ctx.arg.trace && !ltoObjects)
        Msg(ctx) << file;
      if (auto *f = dyn_cast<ObjFile<ELFT>>(file)) {
        ctx.objectFiles.push_back(f);
        for (StringRef lib : f->dependentLibraries)
          addDependentLibrary(ctx, lib, f);
        if (ctx.arg.emachine == EM_ARM)
          f->parseArmAttributes();
      } else if (auto *f = dyn_cast<BitcodeFile>(file)) {
        ctx.bitcodeFiles.push_back(f);
        f->parse();
      } else if (auto *f = dyn_cast<BinaryFile>(file)) {
        ctx.binaryFiles.push_back(f);
      } else if (auto *f = dyn_cast<SharedFile>(file)) {
        f->finishSymbolEvents();
      }
    }
  }

  // The symbol table's order: the batch's new symbols in the order they were
  // first inserted. Symbols that were only looked up (for the references of
  // lazy files that were not extracted, or events of files that add
  // nothing) were never inserted: drop them from the map.
  {
    llvm::TimeTraceScope timeScope("Order symbols");
    std::vector<std::vector<std::pair<Key, Symbol *>>> perShard(
        SymbolTable::numShards);
    parallelFor(0, SymbolTable::numShards, [&](size_t s) {
      BatchShard &shard = shards[s];
      SymbolTable::Shard &tshard = symtab.shard(s);
      for (uint32_t slot = shard.base; slot < shard.info.size(); ++slot)
        if (shard.info[slot].firstKey != UINT64_MAX)
          perShard[s].push_back({shard.info[slot].firstKey, tshard.syms[slot]});
      auto &map = tshard.map;
      SmallVector<CachedHashStringRef, 0> unused;
      for (auto &[stem, entry] : map) {
        unsigned home = entry.home >> SymbolTable::slotBits;
        uint32_t slot = entry.home & ((1u << SymbolTable::slotBits) - 1);
        if (home == s && slot >= shard.base &&
            (slot >= shard.info.size() ||
             shard.info[slot].firstKey == UINT64_MAX))
          unused.push_back(stem);
      }
      for (CachedHashStringRef stem : unused)
        map.erase(stem);
    });
    std::vector<std::pair<Key, Symbol *>> all;
    for (auto &v : perShard)
      all.insert(all.end(), v.begin(), v.end());
    parallelSort(
        all, [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &[key, sym] : all)
      symtab.addToSymVector(sym);
  }

  // --why-extract records: one per extracted file, in the order the
  // one-file-at-a-time parse made them, which is once the extracted file
  // (and everything it extracted) had been parsed.
  if (!ctx.arg.whyExtract.empty()) {
    std::vector<std::pair<uint32_t, WhyExtract *>> ordered;
    for (BatchShard &shard : shards)
      for (WhyExtract &w : shard.whyExtract)
        ordered.push_back(
            {records[parseRecord.lookup(const_cast<InputFile *>(w.extracted))]
                 .subtreeEnd,
             &w});
    llvm::sort(ordered, [](auto &a, auto &b) { return a.first < b.first; });
    for (auto &[end, w] : ordered)
      ctx.whyExtractRecords.emplace_back(toStr(ctx, w->reference), w->extracted,
                                         *w->sym);
  }

  // Diagnostics in event order.
  std::vector<std::pair<Key, Diag *>> diags;
  for (BatchShard &shard : shards)
    for (Diag &diag : shard.diags)
      diags.push_back({records[diag.rec].keyOf(diag.pos), &diag});
  llvm::sort(diags, [](auto &a, auto &b) { return a.first < b.first; });
  for (auto &[key, diag] : diags)
    ELFSyncStream(ctx, diag->level) << diag->msg;

  if (ctx.arg.warnBackrefs) {
    for (unsigned s = 0; s < SymbolTable::numShards; ++s) {
      BatchShard &shard = shards[s];
      for (uint32_t slot = 0; slot < shard.info.size(); ++slot) {
        SymInfo &d = shard.info[slot];
        if (d.head == UINT32_MAX)
          continue;
        Symbol *sym = symtab.shard(s).syms[slot];
        if (d.hasBackref)
          ctx.backwardReferences[sym] = {d.backrefFrom, d.backrefTo};
        else
          ctx.backwardReferences.erase(sym);
      }
    }
  }
}

// --- Driver ------------------------------------------------------------------

template <class ELFT> void Resolver<ELFT>::run(ArrayRef<InputFile *> files) {
  for (unsigned s = 0; s < SymbolTable::numShards; ++s) {
    shards[s].base = symtab.shard(s).syms.size();
    shards[s].info.resize(shards[s].base);
  }
  for (InputFile *file : files) {
    uint32_t r = records.size();
    records.emplace_back(file, file->lazy);
    records[r].root = roots.size();
    roots.push_back(r);
    if (!isCompatible(ctx, file)) {
      records[r].dead = true;
      records[r].incompatible = true;
      continue;
    }
    if (!file->lazy)
      parseRecord[file] = r;
  }
  {
    llvm::TimeTraceScope timeScope("Prepare files");
    prepare(roots);
  }
  {
    llvm::TimeTraceScope timeScope("Look up symbols");
    lightPass();
    if (ctx.arg.warnBackrefs) {
      // The backward-reference state of symbols from before the batch.
      for (unsigned s = 0; s < SymbolTable::numShards; ++s) {
        BatchShard &shard = shards[s];
        for (uint32_t slot = 0; slot < shard.base; ++slot) {
          SymInfo &d = shard.info[slot];
          if (d.head == UINT32_MAX)
            continue;
          auto it = ctx.backwardReferences.find(symtab.shard(s).syms[slot]);
          if (it != ctx.backwardReferences.end()) {
            d.hasBackref = true;
            d.backrefFrom = it->second.first;
            d.backrefTo = it->second.second;
          }
        }
      }
    }
  }
  {
    llvm::TimeTraceScope timeScope("Extract files");
    // While this thread builds the extraction tree, the pool scans the
    // files' COMDAT groups: that needs nothing from resolution, only which
    // files exist. Files that end up not extracted are scanned in vain
    // (~10%); the choice of the kept groups still happens in finish(), after
    // the tree fixes the file order.
    if (!ltoObjects) {
      SmallVector<InputFile *, 0> toScan;
      for (Record &rec : records)
        if (!rec.dead && (isa<ObjFile<ELFT>>(rec.file) ||
                          isa<BitcodeFile>(rec.file)))
          toScan.push_back(rec.file);
      scanGroup.emplace();
      size_t numChunks = std::min<size_t>(
          toScan.size(), 4 * llvm::parallel::getThreadCount());
      for (size_t c = 0; c < numChunks; ++c)
        scanGroup->spawn([this, toScan, c, numChunks] {
          for (size_t i = c; i < toScan.size(); i += numChunks) {
            if (auto *f = dyn_cast<ObjFile<ELFT>>(toScan[i]))
              f->scanComdats();
            else
              cast<BitcodeFile>(toScan[i])->scanComdats();
          }
        });
    }
    rootRequests();
    buildTree();
  }
  {
    llvm::TimeTraceScope timeScope("Resolve");
    renumber();
    replay();
  }
  finish();
}

} // namespace

// --- Event buckets -----------------------------------------------------------

// A counting sort of the events by bucket; the order within a bucket is the
// event order.
void InputFile::SymbolEvents::build(uint32_t n,
                                    function_ref<int(uint32_t)> bucket) {
  constexpr unsigned numBuckets = 2 * SymbolTable::numShards;
  num = n;
  bounds = std::make_unique<uint32_t[]>(numBuckets + 1);
  std::fill_n(bounds.get(), numBuckets + 1, 0);
  for (uint32_t e = 0; e < n; ++e) {
    int b = bucket(e);
    if (b >= 0)
      ++bounds[b + 1];
  }
  for (unsigned b = 0; b < numBuckets; ++b)
    bounds[b + 1] += bounds[b];
  order = std::make_unique<uint32_t[]>(bounds[numBuckets]);
  SmallVector<uint32_t, 0> next(bounds.get(), bounds.get() + numBuckets);
  for (uint32_t e = 0; e < n; ++e) {
    int b = bucket(e);
    if (b >= 0)
      order[next[b]++] = e;
  }
}

// --- Hooks called from Symbol::resolve() ------------------------------------

void SymbolTable::extract(const Symbol &sym, InputFile *file,
                          const InputFile *reference, bool backref) {
  if (ReplayContext *rc = currentContext) {
    // Building the tree: note the request. The replay: the tree has the
    // extraction already; note what to record once its events are in.
    assert(rc->mode == ReplayContext::Scratch || !file->lazy);
    rc->requested = file;
    rc->reference = reference;
    rc->backref = backref;
    return;
  }
  // Outside of a batch (--entry, -u, --undefined-glob, PROVIDE, libcalls):
  // parse the file right away, as a batch of its own.
  file->lazy = false;
  parseFiles(ctx, {file});
  if (reference && !ctx.arg.whyExtract.empty())
    ctx.whyExtractRecords.emplace_back(toStr(ctx, reference), sym.file, sym);
  if (backref && !sym.isWeak())
    ctx.backwardReferences.try_emplace(&sym,
                                       std::make_pair(reference, sym.file));
}

void SymbolTable::dismissBackref(const Symbol &sym) {
  if (ReplayContext *rc = currentContext) {
    if (rc->mode == ReplayContext::Replay)
      rc->shard->info[rc->slot].hasBackref = false;
    return;
  }
  ctx.backwardReferences.erase(&sym);
}

bool SymbolTable::recordDiag(DiagLevel level, StringRef msg) {
  ReplayContext *rc = currentContext;
  if (!rc)
    return false;
  if (rc->mode == ReplayContext::Replay)
    rc->shard->diags.push_back({rc->rec, rc->pos, level, msg.str()});
  return true;
}

ResolveDiag::~ResolveDiag() {
  if (!ctx.symtab->recordDiag(level, os.str()))
    ELFSyncStream(ctx, level) << os.str();
}

// --- Entry point -------------------------------------------------------------

template <class ELFT>
static void doParseFiles(Ctx &ctx, ArrayRef<InputFile *> files,
                         bool ltoObjects) {
  Resolver<ELFT>(ctx, ltoObjects).run(files);
}

void elf::parseFiles(Ctx &ctx, ArrayRef<InputFile *> files, bool ltoObjects) {
  if (files.empty())
    return;
  llvm::TimeTraceScope timeScope("Parse input files");
  invokeELFT(doParseFiles, ctx, files, ltoObjects);
}
