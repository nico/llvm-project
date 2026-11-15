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
  explicit DsymWriter(const MachOFile &Binary) : Binary(Binary) {}

  /// Adds a section of \p Size bytes to the __DWARF segment and returns its
  /// index. Sections are laid out in the order they are added, which is also
  /// the order in which they are written to the file: see finishSection().
  unsigned addSection(StringRef Name, uint64_t Size) {
    Sections.push_back({Name, Size});
    return Sections.size() - 1;
  }

  /// The same for a section whose contents exist already. They have to stay
  /// at \p Contents until commit().
  unsigned addSection(StringRef Name, ArrayRef<char> Contents) {
    Sections.push_back({Name, Contents.size()});
    Sections.back().Data = Contents.data();
    Sections.back().IsComplete = true;
    return Sections.size() - 1;
  }

  /// Creates the bundle \p BundlePath with a Mach-O file called
  /// \p BinaryName in it, and starts to write the file.
  void create(StringRef BundlePath, StringRef BinaryName);

  /// Where the contents of section \p Index go. Valid after create().
  char *sectionData(unsigned Index) { return Sections[Index].Buffer.get(); }

  /// Says that the contents of section \p Index are complete.
  ///
  /// Writing the file takes about as long as filling the sections does, and
  /// is the work of one thread. So the file is written from front to back on
  /// a thread of its own, as far as the sections are complete, while later
  /// sections are filled.
  void finishSection(unsigned Index);

  /// Takes all sections to be complete, waits until the file is written and
  /// gives it its name.
  void commit();

private:
  struct Section {
    StringRef Name;
    uint64_t Size;
    uint64_t FileOffset = 0;
    /// The contents: Buffer.get(), unless they were handed to addSection().
    const char *Data = nullptr;
    std::unique_ptr<char[]> Buffer;
    bool IsComplete = false;
  };

  void writeFile();

  const MachOFile &Binary;
  std::vector<Section> Sections;
  /// The file up to the first section: header, load commands, symbols.
  std::unique_ptr<char[]> Head;
  uint64_t HeadSize = 0;
  std::string Path;
  /// The file has another name until it is complete.
  std::optional<sys::fs::TempFile> File;
  std::error_code WriteError;
  std::thread Thread;
  /// Guards Section::IsComplete.
  std::mutex Mutex;
  std::condition_variable SectionFinished;
};

} // namespace dsymutil2
} // namespace llvm

#endif
