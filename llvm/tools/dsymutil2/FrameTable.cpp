//===- FrameTable.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __debug_frame is a list of entries. A CIE holds what many functions' unwind
// descriptions have in common; an FDE describes one function and names its
// CIE by the CIE's offset in the section. An FDE is position independent but
// for that offset and the address of its function, so it is kept or dropped
// as a whole and patched in those two places.
//
//===----------------------------------------------------------------------===//

#include "FrameTable.h"
#include "ObjectFile.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

// An entry is a 4-byte length, a 4-byte CIE id or pointer, and for an FDE the
// address and the size of its function. Object files this tool reads are
// 64-bit, so those have 8 bytes each.
static const uint64_t FDEHeaderSize = 4 + 4 + 8 + 8;

const char *dsymutil2::linkFrameTable(ObjectFile &Object) {
  StringRef Section = Object.Frame;
  // The CIEs of the section, by offset. Found first: an FDE may come before
  // its CIE.
  SmallVector<std::pair<uint64_t, StringRef>, 4> CIEs;
  for (bool WantCIEs : {true, false}) {
    for (uint64_t Offset = 0; Section.size() - Offset >= 8;) {
      const uint8_t *Entry = Section.bytes_begin() + Offset;
      uint64_t Length = read32le(Entry);
      if (Length == dwarf::DW_LENGTH_DWARF64)
        return "DWARF64 __debug_frame is not supported";
      if (Length < 4 || Length > Section.size() - Offset - 4)
        return "malformed entry in __debug_frame";
      uint64_t Size = 4 + Length;
      uint32_t Id = read32le(Entry + 4);
      bool IsCIE = Id == dwarf::DW_CIE_ID;
      if (IsCIE && WantCIEs) {
        CIEs.emplace_back(Offset, Section.substr(Offset, Size));
      } else if (!IsCIE && !WantCIEs && Size >= FDEHeaderSize) {
        const AddressRange *Range = Object.findContaining(read64le(Entry + 8));
        auto CIE = llvm::find_if(
            CIEs, [&](const auto &Seen) { return Seen.first == Id; });
        if (Range && CIE != CIEs.end()) {
          // Object files often repeat one CIE; keep one of each.
          auto Kept = llvm::find(Object.FrameCIEs, CIE->second);
          if (Kept == Object.FrameCIEs.end()) {
            Object.FrameCIEs.push_back(CIE->second);
            Kept = Object.FrameCIEs.end() - 1;
          }
          size_t Start = Object.FrameData.size();
          Object.FrameData.append(Entry, Entry + Size);
          char *Copy = Object.FrameData.data() + Start;
          // The CIE's index, until the layout of the output is known.
          write32le(Copy + 4, Kept - Object.FrameCIEs.begin());
          write64le(Copy + 8,
                    read64le(Entry + 8) - Range->Begin + Range->Linked);
        }
      }
      Offset += Size;
    }
  }
  return nullptr;
}

void dsymutil2::patchFrameTable(const ObjectFile &Object, char *Out) {
  for (uint64_t Offset = 0; Offset < Object.FrameData.size();) {
    char *Entry = Out + Offset;
    write32le(Entry + 4, Object.FrameCIEOffsets[read32le(Entry + 4)]);
    Offset += 4 + uint64_t(read32le(Entry));
  }
}
