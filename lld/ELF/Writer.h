//===- Writer.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_WRITER_H
#define LLD_ELF_WRITER_H

#include "Config.h"

namespace lld::elf {
class OutputSection;
template <class ELFT> void writeResult(Ctx &ctx);
// Starts allocating and pre-touching the in-memory output buffer on a
// background thread; openFile() adopts it if the real file size fits.
void startOutputBufferPreTouch(Ctx &ctx);

void addReservedSymbols(Ctx &ctx);
bool includeInSymtab(Ctx &, const Symbol &);
unsigned getSectionRank(Ctx &, OutputSection &osec);

} // namespace lld::elf

#endif
