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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace llvm {
namespace dsymutil2 {
class MachOFile;

class DsymWriter {
public:
  /// A section header has 32 bits for where the section is in the file.
  /// \p OffsetLimit is the first offset that it cannot say: 4 GiB, or less
  /// for a test. \p AllowOffsetOverflow says whether to write a file in
  /// which a section starts there or later.
  DsymWriter(const MachOFile &Binary, bool AllowOffsetOverflow,
             uint64_t OffsetLimit)
      : Binary(Binary), AllowOffsetOverflow(AllowOffsetOverflow),
        OffsetLimit(OffsetLimit) {}

  /// Adds a section of \p Size bytes to the __DWARF segment and returns its
  /// index. Sections are laid out in the order they are added, except for
  /// the one given to setLastReachableSection().
  unsigned addSection(StringRef Name, uint64_t Size) {
    Section &S = Sections.emplace_back();
    S.Name = Name;
    S.Size = Size;
    return Sections.size() - 1;
  }

  /// The same for a section whose contents exist already. They have to stay
  /// at \p Contents until commit().
  unsigned addSection(StringRef Name, ArrayRef<char> Contents) {
    unsigned Index = addSection(Name, Contents.size());
    Sections[Index].Data = Contents.data();
    Sections[Index].IsComplete = true;
    return Index;
  }

  /// Says that section \p Index is the one to go across the offset limit,
  /// if the sections do not all fit below it: it goes behind as many of the
  /// other sections as can be in front of it with it still starting below
  /// the limit, and in front of the rest. Readers that take a section
  /// header at its word find it and what is in front of it.
  void setLastReachableSection(unsigned Index) { LastReachable = Index; }

  /// Creates the bundle \p BundlePath with a Mach-O file called
  /// \p BinaryName in it, and starts to write the file.
  void create(StringRef BundlePath, StringRef BinaryName);

  /// Where the contents of section \p Index go. Valid after create().
  char *sectionData(unsigned Index) { return Sections[Index].Buffer.get(); }

  /// Says that the contents of section \p Index are complete.
  ///
  /// Writing the file takes about as long as filling the sections does, and
  /// is the work of one thread. So the file is written on a thread of its
  /// own, a section at a time as they become complete, while the other
  /// sections are filled.
  void finishSection(unsigned Index);

  /// Takes all sections to be complete, waits until the file is written and
  /// gives it its name.
  void commit();

private:
  struct Section {
    StringRef Name;
    uint64_t Size = 0;
    uint64_t FileOffset = 0;
    /// The contents: Buffer.get(), unless they were handed to addSection().
    const char *Data = nullptr;
    std::unique_ptr<char[]> Buffer;
    bool IsComplete = false;
    bool IsWritten = false;
  };

  void writeFile();

  const MachOFile &Binary;
  bool AllowOffsetOverflow;
  uint64_t OffsetLimit;
  std::vector<Section> Sections;
  std::optional<unsigned> LastReachable;
  /// The file up to the first section: header, load commands, symbols.
  std::unique_ptr<char[]> Head;
  uint64_t HeadSize = 0;
  std::string Path;
  /// The file has another name until it is complete.
  std::optional<sys::fs::TempFile> File;
  std::error_code WriteError;
  std::thread Thread;
  /// Guards Section::IsComplete and Section::IsWritten.
  std::mutex Mutex;
  std::condition_variable SectionFinished;
};

} // namespace dsymutil2
} // namespace llvm

#endif
