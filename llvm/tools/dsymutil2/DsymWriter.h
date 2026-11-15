//===- DsymWriter.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Writes the .dSYM bundle: an Info.plist and a Mach-O file of type MH_DSYM
// that has the linked binary's UUID, segments (without their contents,
// except for __eh_frame) and symbols, plus a __DWARF segment with the linked
// debug info.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_DSYMUTIL2_DSYMWRITER_H
#define LLVM_TOOLS_DSYMUTIL2_DSYMWRITER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileOutputBuffer.h"
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace dsymutil2 {
class MachOFile;

class DsymWriter {
public:
  explicit DsymWriter(const MachOFile &Binary) : Binary(Binary) {}

  /// Adds a section of \p Size bytes to the __DWARF segment and returns its
  /// index. Sections are laid out in the order they are added.
  unsigned addSection(StringRef Name, uint64_t Size) {
    Sections.push_back({Name, Size, 0});
    return Sections.size() - 1;
  }

  /// Creates the bundle \p BundlePath with a Mach-O file called
  /// \p BinaryName in it, and writes everything but the contents of the
  /// sections.
  void create(StringRef BundlePath, StringRef BinaryName);

  /// Where the contents of section \p Index go. Valid after create().
  char *sectionData(unsigned Index) {
    return reinterpret_cast<char *>(Buffer->getBufferStart()) +
           Sections[Index].FileOffset;
  }

  void commit();

private:
  struct Section {
    StringRef Name;
    uint64_t Size;
    uint64_t FileOffset;
  };

  const MachOFile &Binary;
  std::vector<Section> Sections;
  std::unique_ptr<FileOutputBuffer> Buffer;
};

} // namespace dsymutil2
} // namespace llvm

#endif
