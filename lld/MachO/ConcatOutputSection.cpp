//===- ConcatOutputSection.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ConcatOutputSection.h"
#include "Config.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include <deque>

using namespace llvm;
using namespace llvm::MachO;
using namespace lld;
using namespace lld::macho;

MapVector<NamePair, ConcatOutputSection *> macho::concatOutputSections;

void ConcatOutputSection::addInput(ConcatInputSection *input) {
  assert(input->parent == this);
  if (inputs.empty()) {
    align = input->align;
    flags = input->getFlags();
  } else {
    align = std::max(align, input->align);
    finalizeFlags(input);
  }
  inputs.push_back(input);
}

// Branch-range extension can be implemented in two ways, either through ...
//
// (1) Branch islands: Single branch instructions (also of limited range),
//     that might be chained in multiple hops to reach the desired
//     destination. On ARM64, as 16 branch islands are needed to hop between
//     opposite ends of a 2 GiB program. LD64 uses branch islands exclusively,
//     even when it needs excessive hops.
//
// (2) Thunks: Instruction(s) to load the destination address into a scratch
//     register, followed by a register-indirect branch. Thunks are
//     constructed to reach any arbitrary address, so need not be
//     chained. Although thunks need not be chained, a program might need
//     multiple thunks to the same destination distributed throughout a large
//     program so that all call sites can have one within range.
//
// The optimal approach is to mix islands for destinations within two hops,
// and use thunks for destinations at greater distance. For now, we only
// implement thunks. TODO: Adding support for branch islands!

DenseMap<ThunkKey, ThunkInfo, ThunkMapKeyInfo> lld::macho::thunkMap;

// Determine whether we need thunks, which depends on the target arch -- RISC
// (i.e., ARM) generally does because it has limited-range branch/call
// instructions, whereas CISC (i.e., x86) generally doesn't. RISC only needs
// thunks for programs so large that branch source & destination addresses
// might differ more than the range of branch instruction(s).
bool TextOutputSection::needsThunks() const {
  if (!target->usesThunks())
    return false;
  // FIXME: It is not enough to just estimate the size of this section. We
  // should compute parent->needsThunks by estimating the size of all __text
  // sections. See https://github.com/llvm/llvm-project/issues/195387
  uint64_t isecAddr = addr;
  for (ConcatInputSection *isec : inputs)
    isecAddr = alignToPowerOf2(isecAddr, isec->align) + isec->getSize();
  // Other sections besides __text might be small enough to pass this
  // test but nevertheless need thunks for calling into other sections.
  // An imperfect heuristic to use in this case is that if a section
  // we've already processed in this segment needs thunks, so do the
  // rest.
  bool needsThunks = parent && parent->needsThunks;

  // Calculate the total size of all branch target sections
  uint64_t branchTargetsSize = in.stubs->getSize();

  // Add the size of __objc_stubs section if it exists
  if (in.objcStubs && in.objcStubs->isNeeded())
    branchTargetsSize += in.objcStubs->getSize();

  if (!needsThunks &&
      isecAddr - addr + branchTargetsSize <=
          std::min(target->backwardBranchRange, target->forwardBranchRange))
    return false;
  // Yes, this program is large enough to need thunks.
  if (parent)
    parent->needsThunks = true;
  return true;
}

void ConcatOutputSection::finalizeOne(ConcatInputSection *isec) {
  size = alignToPowerOf2(size, isec->align);
  fileSize = alignToPowerOf2(fileSize, isec->align);
  isec->outSecOff = size;
  isec->isFinal = true;
  size += isec->getSize();
  fileSize += isec->getFileSize();
}

void ConcatOutputSection::finalizeContents() {
  for (ConcatInputSection *isec : inputs)
    finalizeOne(isec);
}

bool TextOutputSection::isTargetKnownInRange(const ConcatInputSection &isec,
                                             const Relocation &r) const {
  uint64_t callVA = isec.getVA() + r.offset;
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  auto *funcSym = cast<Symbol *>(r.referent);
  uint64_t funcVA = resolveSymbolOffsetVA(funcSym, r.type, r.addend);
  // Check if the referent is reachable with a simple call instruction.
  return lowVA <= funcVA && funcVA <= highVA;
}

Defined *TextOutputSection::createThunk(const ConcatInputSection &isec,
                                        const Relocation &r) {
  auto *thunkIsec =
      makeSyntheticInputSection(isec.getSegName(), isec.getName());
  thunkIsec->parent = this;
  assert(thunkIsec->live);

  std::string addendSuffix;
  if (r.addend != 0)
    addendSuffix = "+" + std::to_string(r.addend);
  size_t thunkSize = target->thunkSize;
  auto *funcSym = cast<Symbol *>(r.referent);
  ThunkInfo &thunkInfo = thunkMap[ThunkKey(r)];
  StringRef thunkName =
      saver().save(funcSym->getName() + addendSuffix + ".thunk." +
                   std::to_string(thunkInfo.sequence++));
  Defined *thunkSym;
  if (!isa<Defined>(funcSym) || cast<Defined>(funcSym)->isExternal()) {
    thunkSym = symtab->addDefined(
        thunkName, /*file=*/nullptr, thunkIsec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isPrivateExtern=*/true,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*isWeakDefCanBeHidden=*/false);
  } else {
    thunkSym = make<Defined>(
        thunkName, /*file=*/nullptr, thunkIsec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isExternal=*/false, /*isPrivateExtern=*/true,
        /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
        /*noDeadStrip=*/false, /*isWeakDefCanBeHidden=*/false);
  }
  thunkSym->used = true;
  target->populateThunk(thunkIsec, funcSym, r.addend);
  thunks.push_back(thunkIsec);
  return thunkSym;
}

std::optional<uint64_t>
TextOutputSection::estimateStubsEndVA(uint64_t endVA) const {
  if (!parent)
    return std::nullopt;

  auto sections =
      ArrayRef(parent->getSections())
          .drop_until([&](const OutputSection *osec) { return osec == this; });

  // Walk backwards to find the last stubs section
  while (!sections.empty()) {
    auto *osec = sections.back();
    if (osec->isNeeded() && (osec == in.stubs || osec == in.objcStubs))
      break;
    sections.consume_back();
  }
  if (sections.empty())
    return std::nullopt;

  uint64_t estimatedStubsEnd = endVA;
  for (auto *osec : sections) {
    if (osec == this)
      continue;
    if (!osec->isNeeded())
      continue;
    // Check if we will emit any more sections before the last stubs section
    if (osec != in.stubs && osec != in.stubHelper && osec != in.objcStubs)
      return std::nullopt;
    estimatedStubsEnd =
        alignToPowerOf2(estimatedStubsEnd, osec->align) + osec->getSize();
  }
  return estimatedStubsEnd;
}

// Whether the branch in \p r goes through __stubs or __objc_stubs. Those
// sections are laid out after this one, so where their entries end up is
// only known by estimateStubsEndVA().
static bool isStubBranch(const Relocation &r) {
  if (r.addend)
    return false;
  auto *funcSym = cast<Symbol *>(r.referent);
  return funcSym->isInStubs() || (in.objcStubs && in.objcStubs->isNeeded() &&
                                  ObjCStubsSection::isObjCStubSymbol(funcSym));
}

// Branches that cannot reach their target get redirected to a thunk that
// can (see TargetInfo::populateThunk()). The thunks sit in "islands"
// between the input sections, one island per `islandSpacing` bytes of
// input; a branch uses the island of the region it is in, which is within
// a fraction of the branch range, and each island holds one thunk per
// target its branches need.
//
// Which branches need a thunk is decided from the layout as it would be
// without thunks, allowing for the thunks to move any section by up to
// `margin` bytes (they add up to less than that, or this is redone with a
// larger margin). That makes the decision independent per branch, so it is
// made in parallel; so is most of the rest. The serial parts only walk
// the few thousand branches that need a thunk, plus flat arrays of the
// input sections' sizes.
void TextOutputSection::finalize() {
  if (!needsThunks()) {
    for (ConcatInputSection *isec : inputs)
      finalizeOne(isec);
    return;
  }

  // The layout is worked out over these arrays rather than the sections
  // themselves, which are scattered in memory. Until finalizeOne() assigns
  // a section's real offset, its outSecOff holds its position in `inputs`.
  TimeTraceScope timeScope("Thunks", name);
  size_t n = inputs.size();
  std::vector<uint32_t> aligns(n);
  std::vector<uint64_t> sizes(n), fileSizes(n), tentativeOffsets(n),
      finalOffsets(n);
  parallelFor(0, n, [&](size_t i) {
    ConcatInputSection *isec = inputs[i];
    aligns[i] = isec->align;
    sizes[i] = isec->getSize();
    fileSizes[i] = isec->getFileSize();
    isec->outSecOff = i;
  });
  uint64_t tentativeEnd = 0;
  for (size_t i = 0; i < n; ++i) {
    tentativeEnd = alignToPowerOf2(tentativeEnd, aligns[i]);
    tentativeOffsets[i] = tentativeEnd;
    tentativeEnd += sizes[i];
  }

  const uint64_t forwardRange = target->forwardBranchRange;
  const uint64_t backwardRange = target->backwardBranchRange;
  const uint64_t islandSpacing = std::min(forwardRange, backwardRange) / 2;
  // Whether a branch at `from` reaches `to` even if thunks move either end
  // by up to `margin` bytes. Addresses that are not assigned yet (see
  // Defined::getVA()) are far away by design.
  auto reaches = [&](uint64_t from, uint64_t to, uint64_t margin) {
    int64_t distance = int64_t(from) - int64_t(to); // > 0 going backward
    return -int64_t(forwardRange - margin) <= distance &&
           distance <= int64_t(backwardRange - margin);
  };

  struct Branch {
    ConcatInputSection *isec; // the caller
    Relocation *r;
    uint32_t callerIndex; // the caller's position in `inputs`
    uint32_t thunkIndex;  // the thunk's position in its island
  };
  struct Island {
    size_t before;   // laid out before this input, or at the end if == n
    size_t farBegin; // the branches using it: a slice of `far`
    size_t farEnd;
    // The first branch to each of the island's thunk targets, in branch
    // order; the thunks are made in this order.
    std::vector<Branch *> firsts;
    std::vector<Defined *> syms;
  };
  std::vector<Branch> far; // the branches that need a thunk, in address order
  std::vector<Island> islands;
  size_t numThunks = 0;

  auto forEachFarBranch = [&](size_t i, uint64_t margin,
                              std::optional<uint64_t> stubsEnd, auto callback) {
    ConcatInputSection *isec = inputs[i];
    // Process relocs by ascending address, i.e., ascending offset within isec
    // FIXME: This property does not hold for object files produced by ld64's
    // `-r` mode.
    assert(is_sorted(isec->relocs, [](Relocation &a, Relocation &b) {
      return a.offset > b.offset;
    }));
    for (Relocation &r : reverse(isec->relocs)) {
      if (!target->hasAttr(r.type, RelocAttrBits::BRANCH))
        continue;
      uint64_t callOff = tentativeOffsets[i] + r.offset;
      bool isFar;
      if (isStubBranch(r)) {
        isFar = !stubsEnd || *stubsEnd > addr + callOff + forwardRange;
      } else {
        const ConcatInputSection *callee = nullptr;
        auto *sym = cast<Symbol *>(r.referent);
        const auto *d = dyn_cast<Defined>(sym);
        if (d && !d->isAbsolute())
          if (const auto *c = dyn_cast<ConcatInputSection>(d->isec()))
            if (c->parent == this)
              callee = c;
        if (callee) {
          // A thunk moves both ends of a branch within one section alike.
          isFar = !reaches(callOff,
                           tentativeOffsets[callee->outSecOff] + d->value +
                               r.addend,
                           callee == isec ? 0 : margin);
        } else {
          uint64_t funcVA = resolveSymbolOffsetVA(sym, r.type, r.addend);
          isFar = !reaches(addr + callOff, funcVA, margin);
        }
      }
      if (isFar)
        callback(Branch{isec, &r, uint32_t(i), 0});
    }
  };

  // Thunks tend to be rare, so the margin starts small, which keeps the
  // number of branches that need a thunk close to the minimum.
  {
    TimeTraceScope timeScope("Find far branches");
    uint64_t margin = 1 << 20;
    for (int attempt = 0;; ++attempt) {
      if (attempt == 8)
        fatal(name + ": cannot find room for the thunks");
      std::optional<uint64_t> stubsEnd =
          estimateStubsEndVA(addr + tentativeEnd + margin);
      // Found per chunk of inputs, then concatenated in chunk order, so that
      // `far` is in address order.
      constexpr size_t chunkSize = 1024;
      size_t numChunks = (n + chunkSize - 1) / chunkSize;
      std::vector<std::vector<Branch>> farPerChunk(numChunks);
      parallelFor(0, numChunks, [&](size_t c) {
        for (size_t i = c * chunkSize, e = std::min(n, (c + 1) * chunkSize);
             i < e; ++i)
          forEachFarBranch(i, margin, stubsEnd, [&](const Branch &b) {
            farPerChunk[c].push_back(b);
          });
      });
      far.clear();
      for (const std::vector<Branch> &chunk : farPerChunk)
        far.insert(far.end(), chunk.begin(), chunk.end());

      // The islands, one per region of `islandSpacing` bytes: each goes
      // before the first input at or past the middle of its region (or before
      // a huge input straddling the middle that would push it past the
      // region's end), and gets the region's branches, which are contiguous
      // in `far`.
      islands.assign(tentativeEnd / islandSpacing + 1, Island{});
      auto callOff = [&](const Branch &b) {
        return tentativeOffsets[b.callerIndex] + b.r->offset;
      };
      auto firstInputAtOrPast = [&](uint64_t off) {
        return llvm::lower_bound(tentativeOffsets, off) -
               tentativeOffsets.begin();
      };
      for (auto [j, island] : llvm::enumerate(islands)) {
        uint64_t regionStart = j * islandSpacing;
        island.before = firstInputAtOrPast(regionStart + islandSpacing / 2);
        uint64_t pos =
            island.before < n ? tentativeOffsets[island.before] : tentativeEnd;
        if (pos > regionStart + islandSpacing)
          --island.before;
        island.farBegin =
            llvm::partition_point(far,
                                  [&](const Branch &b) {
                                    return callOff(b) < j * islandSpacing;
                                  }) -
            far.begin();
        island.farEnd =
            llvm::partition_point(far,
                                  [&](const Branch &b) {
                                    return callOff(b) < (j + 1) * islandSpacing;
                                  }) -
            far.begin();
      }
      parallelForEach(islands, [&](Island &island) {
        DenseMap<ThunkKey, uint32_t, ThunkMapKeyInfo> indexOf;
        for (Branch &b : MutableArrayRef(far).slice(
                 island.farBegin, island.farEnd - island.farBegin)) {
          auto [it, inserted] =
              indexOf.try_emplace(ThunkKey(*b.r), island.firsts.size());
          if (inserted)
            island.firsts.push_back(&b);
          b.thunkIndex = it->second;
        }
      });
      numThunks = 0;
      for (const Island &island : islands)
        numThunks += island.firsts.size();
      // An island may also be padded for the alignment of the input after it.
      uint64_t thunkBytes =
          numThunks * target->thunkSize + islands.size() * target->thunkSize;
      if (thunkBytes <= margin)
        break;
      margin = thunkBytes * 2;
    }
  }

  TimeTraceScope timeScope2("Make thunks");
  for (Island &island : islands)
    for (Branch *b : island.firsts)
      island.syms.push_back(createThunk(*b->isec, *b->r));
  parallelForEach(islands, [&](Island &island) {
    for (Branch &b : MutableArrayRef(far).slice(
             island.farBegin, island.farEnd - island.farBegin)) {
      b.r->referent = island.syms[b.thunkIndex];
      // The thunk itself bakes in the addend, so the call-site reloc must
      // branch to the thunk start with no extra offset.
      b.r->addend = 0;
    }
  });
  thunkCallCount += far.size();

  // The layout: as finalizeOne(), over the arrays, with the islands'
  // thunks (which are in island order in `thunks`) laid out where they go.
  TimeTraceScope timeScope3("Lay out");
  ArrayRef<ConcatInputSection *> pendingThunks = thunks;
  size_t nextIsland = 0;
  for (size_t i = 0; i < n; ++i) {
    while (nextIsland < islands.size() && islands[nextIsland].before == i) {
      for (ConcatInputSection *thunk :
           pendingThunks.take_front(islands[nextIsland].syms.size()))
        finalizeOne(thunk);
      pendingThunks = pendingThunks.drop_front(islands[nextIsland].syms.size());
      ++nextIsland;
    }
    size = alignToPowerOf2(size, aligns[i]);
    fileSize = alignToPowerOf2(fileSize, aligns[i]);
    finalOffsets[i] = size;
    size += sizes[i];
    fileSize += fileSizes[i];
  }
  for (ConcatInputSection *thunk : pendingThunks)
    finalizeOne(thunk);
  parallelFor(0, n, [&](size_t i) {
    inputs[i]->outSecOff = finalOffsets[i];
    inputs[i]->isFinal = true;
  });

  // Every section has its address now: check that each thunk is in range
  // of its branches. Independent checks, so in parallel.
  TimeTraceScope timeScope4("Check thunk ranges");
  std::atomic<const Branch *> outOfRange = nullptr;
  parallelForEach(far, [&](const Branch &b) {
    if (!isTargetKnownInRange(*b.isec, *b.r))
      outOfRange = &b;
  });
  if (const Branch *b = outOfRange) {
    auto *thunk = cast<Symbol *>(b->r->referent);
    fatal(name + ": the branch at 0x" +
          utohexstr(b->isec->getVA() + b->r->offset) + " in " +
          toString(b->isec->getFile()) + " cannot reach " + thunk->getName() +
          " at 0x" + utohexstr(thunk->getVA()));
  }

  if (!thunks.empty())
    log(name + ": Created " + Twine(thunks.size()) + " (" +
        Twine(thunks.size() * target->thunkSize / 1024) +
        " KB) thunks and updated " + Twine(thunkCallCount) + " branch targets");
}

void ConcatOutputSection::writeTo(uint8_t *buf) const {
  // Writer::writeSections() already runs the output sections in parallel, but
  // __text usually holds most of the output, so parallelize within a section
  // too. Each input section writes to its own range of the output buffer.
  parallelForEach(inputs, [buf](ConcatInputSection *isec) {
    isec->writeTo(buf + isec->outSecOff);
  });
}

void TextOutputSection::writeTo(uint8_t *buf) const {
  // Inputs and thunks all write to their own range of the output buffer, so
  // there is no need to interleave the two sorted vectors here.
  parallelForEach(inputs, [buf](ConcatInputSection *isec) {
    isec->writeTo(buf + isec->outSecOff);
  });
  parallelForEach(thunks, [buf](ConcatInputSection *thunk) {
    thunk->writeTo(buf + thunk->outSecOff);
  });
}

void ConcatOutputSection::finalizeFlags(InputSection *input) {
  switch (sectionType(input->getFlags())) {
  default /*type-unspec'ed*/:
    // FIXME: Add additional logic here when supporting emitting obj files.
    break;
  case S_4BYTE_LITERALS:
  case S_8BYTE_LITERALS:
  case S_16BYTE_LITERALS:
  case S_CSTRING_LITERALS:
  case S_ZEROFILL:
  case S_LAZY_SYMBOL_POINTERS:
  case S_MOD_TERM_FUNC_POINTERS:
  case S_THREAD_LOCAL_REGULAR:
  case S_THREAD_LOCAL_ZEROFILL:
  case S_THREAD_LOCAL_VARIABLES:
  case S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
  case S_THREAD_LOCAL_VARIABLE_POINTERS:
  case S_NON_LAZY_SYMBOL_POINTERS:
  case S_SYMBOL_STUBS:
    flags |= input->getFlags();
    break;
  }
}

ConcatOutputSection *
ConcatOutputSection::getOrCreateForInput(const InputSection *isec) {
  NamePair names = maybeRenameSection({isec->getSegName(), isec->getName()});
  ConcatOutputSection *&osec = concatOutputSections[names];
  if (!osec) {
    if (isec->getSegName() == segment_names::text &&
        isec->getName() != section_names::gccExceptTab &&
        isec->getName() != section_names::ehFrame)
      osec = make<TextOutputSection>(names.second);
    else
      osec = make<ConcatOutputSection>(names.second);
  }
  return osec;
}

ConcatOutputSection *
ConcatOutputSection::findForInput(const InputSection *isec) {
  NamePair names = maybeRenameSection({isec->getSegName(), isec->getName()});
  return concatOutputSections.lookup(names);
}

NamePair macho::maybeRenameSection(NamePair key) {
  auto newNames = config->sectionRenameMap.find(key);
  if (newNames != config->sectionRenameMap.end())
    return newNames->second;
  return key;
}
