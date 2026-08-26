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
  return isTargetKnownInRange(isec.getVA() + r.offset, r);
}

bool TextOutputSection::isTargetKnownInRange(uint64_t callVA,
                                             const Relocation &r) const {
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  auto *funcSym = cast<Symbol *>(r.referent);
  uint64_t funcVA = resolveSymbolOffsetVA(funcSym, r.type, r.addend);
  // Check if the referent is reachable with a simple call instruction.
  return lowVA <= funcVA && funcVA <= highVA;
}

// Looks for an already-created thunk that puts the branch back in range.
// Most branches never get a thunk, so this must not add a thunkMap entry: on a
// large link that would grow the map to millions of entries that only ever
// hold a default-constructed ThunkInfo, which getThunkInRange() treats the
// same as a miss anyway.
Defined *TextOutputSection::findThunkInRange(uint64_t callVA,
                                             const Relocation &r) const {
  auto it = thunkMap.find(ThunkKey(r));
  if (it == thunkMap.end())
    return nullptr;
  return getThunkInRange(callVA, r, it->second);
}

Defined *TextOutputSection::getThunkInRange(uint64_t callVA,
                                            const Relocation &r,
                                            const ThunkInfo &thunkInfo) const {
  if (!thunkInfo.sym)
    return nullptr;
  uint64_t lowVA = target->backwardBranchRange < callVA
                       ? callVA - target->backwardBranchRange
                       : 0;
  uint64_t highVA = callVA + target->forwardBranchRange;
  uint64_t thunkVA = thunkInfo.isec->getVA();
  if (lowVA <= thunkVA && thunkVA <= highVA)
    return thunkInfo.sym;
  return nullptr;
}

void TextOutputSection::updateBranchTargetToThunk(Relocation &r,
                                                  Defined *thunk) {
  r.referent = thunk;
  // The thunk itself bakes in the addend, so the call-site reloc must
  // branch to the thunk start with no extra offset.
  r.addend = 0;
  ++thunkCallCount;
}

void TextOutputSection::createThunk(const ConcatInputSection &isec,
                                    uint64_t callVA, Relocation &r,
                                    ThunkInfo &thunkInfo) {
  assert(getThunkInRange(callVA, r, thunkInfo) == nullptr);
  uint64_t highVA = callVA + target->forwardBranchRange;
  if (addr + size > highVA) {
    // There were too many consecutive branch instructions for `slop`
    // below. If you hit this: For the current algorithm, just bumping up
    // slop below and trying again is probably simplest. (See also PR51578
    // comment 5).
    fatal(Twine(__FUNCTION__) +
          ": FIXME: thunk range overrun. Consider increasing the "
          "slop-scale with `--slop-scale=<unsigned_int>`.");
  }
  thunkInfo.isec = makeSyntheticInputSection(isec.getSegName(), isec.getName());
  thunkInfo.isec->parent = this;
  assert(thunkInfo.isec->live);

  std::string addendSuffix;
  if (r.addend != 0)
    addendSuffix = "+" + std::to_string(r.addend);
  size_t thunkSize = target->thunkSize;
  auto *funcSym = cast<Symbol *>(r.referent);
  StringRef thunkName =
      saver().save(funcSym->getName() + addendSuffix + ".thunk." +
                   std::to_string(thunkInfo.sequence++));
  if (!isa<Defined>(funcSym) || cast<Defined>(funcSym)->isExternal()) {
    thunkInfo.sym = symtab->addDefined(
        thunkName, /*file=*/nullptr, thunkInfo.isec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isPrivateExtern=*/true,
        /*isReferencedDynamically=*/false, /*noDeadStrip=*/false,
        /*isWeakDefCanBeHidden=*/false);
  } else {
    thunkInfo.sym = make<Defined>(
        thunkName, /*file=*/nullptr, thunkInfo.isec, /*value=*/0, thunkSize,
        /*isWeakDef=*/false, /*isExternal=*/false, /*isPrivateExtern=*/true,
        /*includeInSymtab=*/true, /*isReferencedDynamically=*/false,
        /*noDeadStrip=*/false, /*isWeakDefCanBeHidden=*/false);
  }
  thunkInfo.sym->used = true;
  // Thunks are keyed by symbol, or by section and offset for defined symbols
  // (aliases share a thunk), see ThunkKey; note both for mayHaveThunk().
  thunkedSymbols.insert(funcSym);
  if (auto *d = dyn_cast<Defined>(funcSym))
    if (InputSection *targetIsec = d->isec())
      thunkedSections.insert(targetIsec);
  target->populateThunk(thunkInfo.isec, funcSym, r.addend);
  updateBranchTargetToThunk(r, thunkInfo.sym);
  finalizeOne(thunkInfo.isec);
  thunks.push_back(thunkInfo.isec);
}

std::optional<uint64_t>
TextOutputSection::estimateStubsEndVA(unsigned numPotentialThunks) const {
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

  assert(inputs.empty() || inputs.back()->isFinal);
  uint64_t estimatedStubsEnd =
      addr + size + numPotentialThunks * target->thunkSize;
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

bool TextOutputSection::isTargetStubsAndInRange(
    const ConcatInputSection &isec, const Relocation &r,
    std::optional<uint64_t> estimatedStubsEnd) const {
  if (!estimatedStubsEnd.has_value())
    return false;
  auto *funcSym = cast<Symbol *>(r.referent);
  if (!funcSym->isInStubs() && !(in.objcStubs && in.objcStubs->isNeeded() &&
                                 ObjCStubsSection::isObjCStubSymbol(funcSym)))
    return false;
  if (r.addend)
    return false;
  uint64_t highVA = isec.getVA() + r.offset + target->forwardBranchRange;
  return *estimatedStubsEnd <= highVA;
}

void TextOutputSection::finalize() {
  if (!needsThunks()) {
    for (ConcatInputSection *isec : inputs)
      finalizeOne(isec);
    return;
  }

  // A branch the loop below has to look at: its relocation, and what the
  // parallel pass ahead of the loop found out about it, so that the loop
  // does not have to chase the target symbol for the millions of branches
  // to a section that is simply not laid out yet.
  struct Branch {
    ConcatInputSection *isec; // the caller
    Relocation *r;
    Symbol *sym;
    // The target's section if it is one of this section's inputs, else null.
    const ConcatInputSection *callee;
    uint32_t callerIndex; // the caller's position in `inputs`
    uint32_t calleeIndex; // the callee's position in `inputs`
    // The target's offset in `callee` (symbol value plus addend), if the
    // branch resolves to an address in `callee` (a defined symbol not
    // going through a stub). Then the branch is in range or not by the
    // layout alone, without touching the symbol.
    bool viaCallee;
    int64_t targetOffset;
  };
  // Branches whose target sections are out of range or have not yet been
  // finalized. We may need to emit thunks for them.
  std::deque<Branch *> branchesToProcess;
  // Branches whose targets have not yet be finalized, but a thunk for that
  // target exists. We defer processing these branches because it's possible we
  // can still direct call to their targets after they have all been finalized.
  SmallVector<std::pair<Branch *, Defined *>> deferredBranchRedirects;

  // Nearly all branches are to a section laid out earlier and well within
  // range, and the loop below has nothing to do for those. It looked at
  // every branch to find that out, though, at ~35 ns each. So decide it ahead
  // of time, in parallel, from the layout as it would be without thunks: a
  // thunk pushes everything after it back by a few bytes, so two sections
  // that were more than `margin` inside the range stay in range as long as
  // the thunks add up to less than that (which is checked afterwards). Until
  // finalizeOne() assigns a section's real offset, its outSecOff holds its
  // position in `inputs`.
  const uint64_t margin =
      std::min(target->backwardBranchRange, target->forwardBranchRange) / 8;
  // The layout is worked out over these arrays rather than the sections
  // themselves, which are scattered in memory: the loop below is serial, and
  // touching 746k sections in it is what it would mostly be doing otherwise.
  // The results are written back to the sections afterwards, in parallel.
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
  {
    uint64_t off = 0;
    for (size_t i = 0; i < n; ++i) {
      off = alignToPowerOf2(off, aligns[i]);
      tentativeOffsets[i] = off;
      off += sizes[i];
    }
  }
  auto surelyInRange = [&](const Branch &b) {
    if (!b.viaCallee)
      return false;
    int64_t callOff = tentativeOffsets[b.callerIndex] + b.r->offset;
    int64_t targetOff = tentativeOffsets[b.calleeIndex] + b.targetOffset;
    int64_t distance = callOff - targetOff; // positive for backward branches
    if (b.callee == b.isec) // A thunk moves both ends the same way.
      return -int64_t(target->forwardBranchRange) <= distance &&
             distance <= int64_t(target->backwardBranchRange);
    // Only sections laid out before the caller have an address when the
    // caller is looked at; those move by no more than the caller does.
    if (b.calleeIndex >= b.callerIndex)
      return false;
    return -int64_t(target->forwardBranchRange) <= distance &&
           distance + int64_t(margin) <= int64_t(target->backwardBranchRange);
  };
  // The branches the loop has to look at, in the order it looks at them: by
  // section, and by descending offset within one. One flat array, so that
  // the loop reads them sequentially: found in parallel per section, then
  // counted, then placed.
  auto forEachBranch = [&](size_t i, auto callback) {
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
      Branch b{isec,        &r,    cast<Symbol *>(r.referent), nullptr,
               uint32_t(i), UINT32_MAX, false, 0};
      if (const auto *d = dyn_cast<Defined>(b.sym))
        if (!d->isAbsolute())
          if (const auto *callee = dyn_cast<ConcatInputSection>(d->isec()))
            if (callee->parent == this) {
              b.callee = callee;
              b.calleeIndex = callee->outSecOff;
              // See resolveSymbolOffsetVA().
              b.viaCallee = !(r.addend == 0 && d->isInStubs());
              b.targetOffset = d->value + r.addend;
            }
      if (!surelyInRange(b))
        callback(b);
    }
  };
  std::vector<uint32_t> branchStart(inputs.size() + 1, 0);
  parallelFor(0, inputs.size(), [&](size_t i) {
    forEachBranch(i, [&](const Branch &) { ++branchStart[i + 1]; });
  });
  for (size_t i = 0; i < inputs.size(); ++i)
    branchStart[i + 1] += branchStart[i];
  std::vector<Branch> branches(branchStart.back());
  parallelFor(0, inputs.size(), [&](size_t i) {
    Branch *out = &branches[branchStart[i]];
    forEachBranch(i, [&](const Branch &b) { *out++ = b; });
  });

  // The branch's address, once its section is laid out.
  auto callVAOf = [&](const Branch &b) {
    return addr + finalOffsets[b.callerIndex] + b.r->offset;
  };
  // isTargetKnownInRange() from the arrays. `laidOut` is how many inputs
  // are laid out so far. A target among the inputs that is not laid out yet
  // is out of range by definition, see Defined::getVA(); a target going
  // through a stub, or outside the inputs, is looked up as usual (nothing
  // among the inputs is touched for those).
  auto inRange = [&](const Branch &b, size_t laidOut) {
    if (b.callee && b.calleeIndex >= laidOut)
      return false;
    uint64_t callVA = callVAOf(b);
    if (!b.viaCallee)
      return isTargetKnownInRange(callVA, *b.r);
    uint64_t lowVA = target->backwardBranchRange < callVA
                         ? callVA - target->backwardBranchRange
                         : 0;
    uint64_t highVA = callVA + target->forwardBranchRange;
    uint64_t funcVA = addr + finalOffsets[b.calleeIndex] + b.targetOffset;
    return lowVA <= funcVA && funcVA <= highVA;
  };

  const uint64_t slop = config->slopScale * target->thunkSize;
  for (size_t i = 0; i < n; ++i) {
    while (!branchesToProcess.empty()) {
      Branch &b = *branchesToProcess.front();
      if (inRange(b, i)) {
        branchesToProcess.pop_front();
        continue;
      }
      if (mayHaveThunk(b.sym, b.callee)) {
        if (auto *thunk = findThunkInRange(callVAOf(b), *b.r)) {
          deferredBranchRedirects.emplace_back(&b, thunk);
          branchesToProcess.pop_front();
          continue;
        }
      }
      uint64_t highVA = callVAOf(b) + target->forwardBranchRange;
      uint64_t nextEnd = alignToPowerOf2(addr + size, aligns[i]) + sizes[i];
      // If we were to emit this section, would we have enough space for more
      // thunks? If we do, then we can delay processing this thunk so we may
      // finalize more potencial target sections. Otherwise we must emit thunks
      // until we have enough space.
      if (nextEnd + slop <= highVA)
        break;

      createThunk(*b.isec, callVAOf(b), *b.r, thunkMap[*b.r]);
      branchesToProcess.pop_front();
    }
    // As finalizeOne(), over the arrays.
    size = alignToPowerOf2(size, aligns[i]);
    fileSize = alignToPowerOf2(fileSize, aligns[i]);
    finalOffsets[i] = size;
    size += sizes[i];
    fileSize += fileSizes[i];

    for (Branch &b : MutableArrayRef(branches).slice(
             branchStart[i], branchStart[i + 1] - branchStart[i])) {
      if (inRange(b, i + 1))
        continue;
      if (mayHaveThunk(b.sym, b.callee)) {
        if (auto *thunk = findThunkInRange(callVAOf(b), *b.r)) {
          deferredBranchRedirects.emplace_back(&b, thunk);
          continue;
        }
      }
      branchesToProcess.emplace_back(&b);
    }
  }
  parallelFor(0, n, [&](size_t i) {
    inputs[i]->outSecOff = finalOffsets[i];
    inputs[i]->isFinal = true;
  });

  // Did the thunks add up to less than the margin, so that every branch
  // skipped above is in range? The last section has moved the most.
  {
    uint64_t shift = 0;
    for (size_t i = 0; i < n; ++i)
      shift = std::max(shift, finalOffsets[i] - tentativeOffsets[i]);
    if (shift > margin)
      fatal(name + ": thunks moved sections by " + Twine(shift) +
            " bytes, more than the " + Twine(margin) +
            " bytes the branch range estimate allowed for");
  }

  // Every section has its address now, so these checks are independent of
  // each other; there are millions of them, so do them in parallel.
  {
    std::vector<uint8_t> inRange(branchesToProcess.size());
    parallelFor(0, branchesToProcess.size(), [&](size_t i) {
      Branch &b = *branchesToProcess[i];
      inRange[i] = isTargetKnownInRange(*b.isec, *b.r);
    });
    size_t i = 0;
    llvm::erase_if(branchesToProcess, [&](auto &) { return inRange[i++]; });
  }
  // Count distinct unresolved branch targets that still lack an in-range thunk.
  // We use this as an upper bound on the number of thunks we may still create
  // when estimating where __stubs / __objc_stubs could end up.
  DenseSet<ThunkKey, ThunkMapKeyInfo> branchTargets;
  for (Branch *b : branchesToProcess) {
    if (!mayHaveThunk(b->sym, b->callee) ||
        !findThunkInRange(b->isec->getVA() + b->r->offset, *b->r))
      branchTargets.insert(ThunkKey(*b->r));
  }

  auto estimatedStubsEnd = estimateStubsEndVA(branchTargets.size());
  for (auto [b, thunk] : deferredBranchRedirects) {
    if (isTargetKnownInRange(*b->isec, *b->r))
      continue;
    if (isTargetStubsAndInRange(*b->isec, *b->r, estimatedStubsEnd))
      continue;
    updateBranchTargetToThunk(*b->r, thunk);
  }

  for (Branch *b : branchesToProcess) {
    if (isTargetStubsAndInRange(*b->isec, *b->r, estimatedStubsEnd))
      continue;
    auto &thunkInfo = thunkMap[*b->r];
    uint64_t callVA = b->isec->getVA() + b->r->offset;
    if (auto *thunk = getThunkInRange(callVA, *b->r, thunkInfo)) {
      updateBranchTargetToThunk(*b->r, thunk);
      continue;
    }
    createThunk(*b->isec, callVA, *b->r, thunkInfo);
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

NamePair macho::maybeRenameSection(NamePair key) {
  auto newNames = config->sectionRenameMap.find(key);
  if (newNames != config->sectionRenameMap.end())
    return newNames->second;
  return key;
}
