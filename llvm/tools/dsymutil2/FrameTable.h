//===- FrameTable.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_FRAMETABLE_H
#define LLVM_TOOLS_DSYMUTIL2_FRAMETABLE_H

namespace llvm {
namespace dsymutil2 {
struct ObjectFile;

/// Collects from \p Object's __debug_frame the frame descriptions (FDEs) of
/// the functions that are in the binary, with their linked addresses, and
/// the common information entries (CIEs) they use: fills in the object's
/// FrameData and FrameCIEs. Returns null on success and what is wrong with
/// the section otherwise.
const char *linkFrameTable(ObjectFile &Object);

/// Replaces the CIE indices in the copy of the object's FrameData at \p Out
/// with the CIEs' offsets in the output section.
void patchFrameTable(const ObjectFile &Object, char *Out);

} // namespace dsymutil2
} // namespace llvm

#endif
