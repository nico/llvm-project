//===- MSFBuilder.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/MSF/MSFError.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamWriter.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>
#if defined(_WIN32)
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::msf;
using namespace llvm::support;

static const uint32_t kSuperBlockBlock = 0;
static const uint32_t kFreePageMap0Block = 1;
static const uint32_t kFreePageMap1Block = 2;
static const uint32_t kNumReservedPages = 3;

static const uint32_t kDefaultFreePageMap = kFreePageMap1Block;
static const uint32_t kDefaultBlockMapAddr = kNumReservedPages;

MSFBuilder::MSFBuilder(uint32_t BlockSize, uint32_t MinBlockCount, bool CanGrow,
                       BumpPtrAllocator &Allocator)
    : Allocator(Allocator), IsGrowable(CanGrow),
      FreePageMap(kDefaultFreePageMap), BlockSize(BlockSize),
      BlockMapAddr(kDefaultBlockMapAddr), FreeBlocks(MinBlockCount, true) {
  FreeBlocks[kSuperBlockBlock] = false;
  FreeBlocks[kFreePageMap0Block] = false;
  FreeBlocks[kFreePageMap1Block] = false;
  FreeBlocks[BlockMapAddr] = false;
}

Expected<MSFBuilder> MSFBuilder::create(BumpPtrAllocator &Allocator,
                                        uint32_t BlockSize,
                                        uint32_t MinBlockCount, bool CanGrow) {
  if (!isValidBlockSize(BlockSize))
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "The requested block size is unsupported");

  return MSFBuilder(BlockSize,
                    std::max(MinBlockCount, msf::getMinimumBlockCount()),
                    CanGrow, Allocator);
}

Error MSFBuilder::setBlockMapAddr(uint32_t Addr) {
  if (Addr == BlockMapAddr)
    return Error::success();

  if (Addr >= FreeBlocks.size()) {
    if (!IsGrowable)
      return make_error<MSFError>(msf_error_code::insufficient_buffer,
                                  "Cannot grow the number of blocks");
    FreeBlocks.resize(Addr + 1, true);
  }

  if (!isBlockFree(Addr))
    return make_error<MSFError>(
        msf_error_code::block_in_use,
        "Requested block map address is already in use");
  freeBlock(BlockMapAddr);
  FreeBlocks[Addr] = false;
  BlockMapAddr = Addr;
  return Error::success();
}

void MSFBuilder::setFreePageMap(uint32_t Fpm) { FreePageMap = Fpm; }

void MSFBuilder::setUnknown1(uint32_t Unk1) { Unknown1 = Unk1; }

Error MSFBuilder::setDirectoryBlocksHint(ArrayRef<uint32_t> DirBlocks) {
  for (auto B : DirectoryBlocks)
    freeBlock(B);
  for (auto B : DirBlocks) {
    if (!isBlockFree(B)) {
      return make_error<MSFError>(msf_error_code::unspecified,
                                  "Attempt to reuse an allocated block");
    }
    FreeBlocks[B] = false;
  }

  DirectoryBlocks = DirBlocks;
  return Error::success();
}

// Grows the file by `NumBlocks` usable blocks, and returns the first of them.
uint32_t MSFBuilder::growBlocks(uint32_t NumBlocks) {
  uint32_t OldBlockCount = FreeBlocks.size();
  uint32_t NewBlockCount = NumBlocks + OldBlockCount;
  uint32_t NextFpmBlock = alignTo(OldBlockCount, BlockSize) + 1;
  FreeBlocks.resize(NewBlockCount, true);
  // If we crossed over an fpm page, we actually need to allocate 2 extra
  // blocks for each FPM group crossed and mark both blocks from the group as
  // used.  FPM blocks are marked as allocated regardless of whether or not
  // they ultimately describe the status of blocks in the file.  This means
  // that not only are extraneous blocks at the end of the main FPM marked as
  // allocated, but also blocks from the alternate FPM are always marked as
  // allocated.
  while (NextFpmBlock < NewBlockCount) {
    NewBlockCount += 2;
    FreeBlocks.resize(NewBlockCount, true);
    FreeBlocks.reset(NextFpmBlock, NextFpmBlock + 2);
    NextFpmBlock += BlockSize;
  }
  return OldBlockCount;
}

void MSFBuilder::freeBlock(uint32_t Block) {
  FreeBlocks[Block] = true;
  FirstFreeBlock = std::min(FirstFreeBlock, Block);
}

Error MSFBuilder::allocateBlocks(uint32_t NumBlocks,
                                 MutableArrayRef<uint32_t> Blocks) {
  if (NumBlocks == 0)
    return Error::success();

  if (!IsGrowable && FreeBlocks.count() < NumBlocks)
    return make_error<MSFError>(msf_error_code::insufficient_buffer,
                                "There are no free Blocks in the file");

  // The lowest free blocks, growing the file when they run out. Nothing is
  // free below FirstFreeBlock, so the search starts there, and everything
  // up to the last block handed out is in use afterwards.
  int Block = FreeBlocks.find_first_in(FirstFreeBlock, FreeBlocks.size());
  for (uint32_t I = 0; I < NumBlocks; ++I) {
    if (Block == -1) {
      uint32_t First = growBlocks(NumBlocks - I);
      Block = FreeBlocks.find_first_in(First, FreeBlocks.size());
    }
    Blocks[I] = Block;
    FreeBlocks.reset(Block);
    Block = FreeBlocks.find_next(Block);
  }
  FirstFreeBlock = Blocks[NumBlocks - 1] + 1;
  return Error::success();
}

uint32_t MSFBuilder::getNumUsedBlocks() const {
  return getTotalBlockCount() - getNumFreeBlocks();
}

uint32_t MSFBuilder::getNumFreeBlocks() const { return FreeBlocks.count(); }

uint32_t MSFBuilder::getTotalBlockCount() const { return FreeBlocks.size(); }

bool MSFBuilder::isBlockFree(uint32_t Idx) const { return FreeBlocks[Idx]; }

Expected<uint32_t> MSFBuilder::addStream(uint32_t Size,
                                         ArrayRef<uint32_t> Blocks) {
  // Add a new stream mapped to the specified blocks.  Verify that the specified
  // blocks are both necessary and sufficient for holding the requested number
  // of bytes, and verify that all requested blocks are free.
  uint32_t ReqBlocks = bytesToBlocks(Size, BlockSize);
  if (ReqBlocks != Blocks.size())
    return make_error<MSFError>(
        msf_error_code::invalid_format,
        "Incorrect number of blocks for requested stream size");
  for (auto Block : Blocks) {
    if (Block >= FreeBlocks.size())
      FreeBlocks.resize(Block + 1, true);

    if (!FreeBlocks.test(Block))
      return make_error<MSFError>(
          msf_error_code::unspecified,
          "Attempt to re-use an already allocated block");
  }
  // Mark all the blocks occupied by the new stream as not free.
  for (auto Block : Blocks) {
    FreeBlocks.reset(Block);
  }
  StreamData.push_back(std::make_pair(Size, Blocks));
  return StreamData.size() - 1;
}

Expected<uint32_t> MSFBuilder::addStream(uint32_t Size) {
  uint32_t ReqBlocks = bytesToBlocks(Size, BlockSize);
  std::vector<uint32_t> NewBlocks;
  NewBlocks.resize(ReqBlocks);
  if (auto EC = allocateBlocks(ReqBlocks, NewBlocks))
    return std::move(EC);
  StreamData.push_back(std::make_pair(Size, NewBlocks));
  return StreamData.size() - 1;
}

Error MSFBuilder::setStreamSize(uint32_t Idx, uint32_t Size) {
  uint32_t OldSize = getStreamSize(Idx);
  if (OldSize == Size)
    return Error::success();

  uint32_t NewBlocks = bytesToBlocks(Size, BlockSize);
  uint32_t OldBlocks = bytesToBlocks(OldSize, BlockSize);

  if (NewBlocks > OldBlocks) {
    uint32_t AddedBlocks = NewBlocks - OldBlocks;
    // If we're growing, we have to allocate new Blocks.
    std::vector<uint32_t> AddedBlockList;
    AddedBlockList.resize(AddedBlocks);
    if (auto EC = allocateBlocks(AddedBlocks, AddedBlockList))
      return EC;
    auto &CurrentBlocks = StreamData[Idx].second;
    llvm::append_range(CurrentBlocks, AddedBlockList);
  } else if (OldBlocks > NewBlocks) {
    // For shrinking, free all the Blocks in the Block map, update the stream
    // data, then shrink the directory.
    uint32_t RemovedBlocks = OldBlocks - NewBlocks;
    auto CurrentBlocks = ArrayRef<uint32_t>(StreamData[Idx].second);
    auto RemovedBlockList = CurrentBlocks.drop_front(NewBlocks);
    for (auto P : RemovedBlockList)
      freeBlock(P);
    StreamData[Idx].second = CurrentBlocks.drop_back(RemovedBlocks);
  }

  StreamData[Idx].first = Size;
  return Error::success();
}

uint32_t MSFBuilder::getNumStreams() const { return StreamData.size(); }

uint32_t MSFBuilder::getStreamSize(uint32_t StreamIdx) const {
  return StreamData[StreamIdx].first;
}

ArrayRef<uint32_t> MSFBuilder::getStreamBlocks(uint32_t StreamIdx) const {
  return StreamData[StreamIdx].second;
}

uint32_t MSFBuilder::computeDirectoryByteSize() const {
  // The directory has the following layout, where each item is a ulittle32_t:
  //    NumStreams
  //    StreamSizes[NumStreams]
  //    StreamBlocks[NumStreams][]
  uint32_t Size = sizeof(ulittle32_t);             // NumStreams
  Size += StreamData.size() * sizeof(ulittle32_t); // StreamSizes
  for (const auto &D : StreamData) {
    uint32_t ExpectedNumBlocks = bytesToBlocks(D.first, BlockSize);
    assert(ExpectedNumBlocks == D.second.size() &&
           "Unexpected number of blocks");
    Size += ExpectedNumBlocks * sizeof(ulittle32_t);
  }
  return Size;
}

Expected<MSFLayout> MSFBuilder::generateLayout() {
  llvm::TimeTraceScope timeScope("MSF: Generate layout");

  SuperBlock *SB = Allocator.Allocate<SuperBlock>();
  MSFLayout L;
  L.SB = SB;

  std::memcpy(SB->MagicBytes, Magic, sizeof(Magic));
  SB->BlockMapAddr = BlockMapAddr;
  SB->BlockSize = BlockSize;
  SB->NumDirectoryBytes = computeDirectoryByteSize();
  SB->FreeBlockMapBlock = FreePageMap;
  SB->Unknown1 = Unknown1;

  uint32_t NumDirectoryBlocks = bytesToBlocks(SB->NumDirectoryBytes, BlockSize);
  if (NumDirectoryBlocks > DirectoryBlocks.size()) {
    // Our hint wasn't enough to satisfy the entire directory.  Allocate
    // remaining pages.
    std::vector<uint32_t> ExtraBlocks;
    uint32_t NumExtraBlocks = NumDirectoryBlocks - DirectoryBlocks.size();
    ExtraBlocks.resize(NumExtraBlocks);
    if (auto EC = allocateBlocks(NumExtraBlocks, ExtraBlocks))
      return std::move(EC);
    llvm::append_range(DirectoryBlocks, ExtraBlocks);
  } else if (NumDirectoryBlocks < DirectoryBlocks.size()) {
    uint32_t NumUnnecessaryBlocks = DirectoryBlocks.size() - NumDirectoryBlocks;
    for (auto B :
         ArrayRef<uint32_t>(DirectoryBlocks).drop_back(NumUnnecessaryBlocks))
      freeBlock(B);
    DirectoryBlocks.resize(NumDirectoryBlocks);
  }

  // Don't set the number of blocks in the file until after allocating Blocks
  // for the directory, since the allocation might cause the file to need to
  // grow.
  SB->NumBlocks = FreeBlocks.size();

  ulittle32_t *DirBlocks = Allocator.Allocate<ulittle32_t>(NumDirectoryBlocks);
  llvm::uninitialized_copy(DirectoryBlocks, DirBlocks);
  L.DirectoryBlocks = ArrayRef<ulittle32_t>(DirBlocks, NumDirectoryBlocks);

  // The stream sizes should be re-allocated as a stable pointer and the stream
  // map should have each of its entries allocated as a separate stable pointer.
  if (!StreamData.empty()) {
    ulittle32_t *Sizes = Allocator.Allocate<ulittle32_t>(StreamData.size());
    L.StreamSizes = ArrayRef<ulittle32_t>(Sizes, StreamData.size());
    L.StreamMap.resize(StreamData.size());
    for (uint32_t I = 0; I < StreamData.size(); ++I) {
      Sizes[I] = StreamData[I].first;
      ulittle32_t *BlockList =
          Allocator.Allocate<ulittle32_t>(StreamData[I].second.size());
      llvm::uninitialized_copy(StreamData[I].second, BlockList);
      L.StreamMap[I] =
          ArrayRef<ulittle32_t>(BlockList, StreamData[I].second.size());
    }
  }

  L.FreePageMap = FreeBlocks;

  return L;
}

static void commitFpm(WritableBinaryStream &MsfBuffer, const MSFLayout &Layout,
                      BumpPtrAllocator &Allocator) {
  auto FpmStream =
      WritableMappedBlockStream::createFpmStream(Layout, MsfBuffer, Allocator);

  // We only need to create the alt fpm stream so that it gets initialized.
  WritableMappedBlockStream::createFpmStream(Layout, MsfBuffer, Allocator,
                                             true);

  uint32_t BI = 0;
  BinaryStreamWriter FpmWriter(*FpmStream);
  while (BI < Layout.SB->NumBlocks) {
    uint8_t ThisByte = 0;
    for (uint32_t I = 0; I < 8; ++I) {
      bool IsFree =
          (BI < Layout.SB->NumBlocks) ? Layout.FreePageMap.test(BI) : true;
      uint8_t Mask = uint8_t(IsFree) << I;
      ThisByte |= Mask;
      ++BI;
    }
    cantFail(FpmWriter.writeObject(ThisByte));
  }
  assert(FpmWriter.bytesRemaining() == 0);
}

Expected<std::unique_ptr<MSFOutputStream>> MSFBuilder::commit(StringRef Path,
                                                  MSFLayout &Layout) {
  llvm::TimeTraceScope timeScope("Commit MSF");

  Expected<MSFLayout> L = generateLayout();
  if (!L)
    return L.takeError();

  Layout = std::move(*L);

  uint64_t FileSize = uint64_t(Layout.SB->BlockSize) * Layout.SB->NumBlocks;
  // Ensure that the file size is under the limit for the specified block size.
  if (FileSize > getMaxFileSizeFromBlockSize(Layout.SB->BlockSize)) {
    msf_error_code error_code = [](uint32_t BlockSize) {
      switch (BlockSize) {
      case 8192:
        return msf_error_code::size_overflow_8192;
      case 16384:
        return msf_error_code::size_overflow_16384;
      case 32768:
        return msf_error_code::size_overflow_32768;
      default:
        return msf_error_code::size_overflow_4096;
      }
    }(Layout.SB->BlockSize);

    return make_error<MSFError>(
        error_code,
        formatv("File size {0,1:N} too large for current PDB page size {1}",
                FileSize, Layout.SB->BlockSize));
  }

  uint64_t NumDirectoryBlocks =
      bytesToBlocks(Layout.SB->NumDirectoryBytes, Layout.SB->BlockSize);
  uint64_t DirectoryBlockMapSize =
      NumDirectoryBlocks * sizeof(support::ulittle32_t);
  if (DirectoryBlockMapSize > Layout.SB->BlockSize) {
    return make_error<MSFError>(msf_error_code::stream_directory_overflow,
                                formatv("The directory block map ({0} bytes) "
                                        "doesn't fit in a block ({1} bytes)",
                                        DirectoryBlockMapSize,
                                        Layout.SB->BlockSize));
  }

  Expected<std::unique_ptr<MSFOutputStream>> OutOrError =
      MSFOutputStream::create(Path, FileSize, Layout.SB->BlockSize);
  if (auto EC = OutOrError.takeError())
    return std::move(EC);
  MSFOutputStream &Buffer = **OutOrError;
  BinaryStreamWriter Writer(Buffer);

  if (auto EC = Writer.writeObject(*Layout.SB))
    return std::move(EC);

  commitFpm(Buffer, Layout, Allocator);

  uint32_t BlockMapOffset =
      msf::blockToOffset(Layout.SB->BlockMapAddr, Layout.SB->BlockSize);
  Writer.setOffset(BlockMapOffset);
  if (auto EC = Writer.writeArray(Layout.DirectoryBlocks))
    return std::move(EC);

  auto DirStream = WritableMappedBlockStream::createDirectoryStream(
      Layout, Buffer, Allocator);
  BinaryStreamWriter DW(*DirStream);
  if (auto EC = DW.writeInteger<uint32_t>(Layout.StreamSizes.size()))
    return std::move(EC);

  if (auto EC = DW.writeArray(Layout.StreamSizes))
    return std::move(EC);

  for (const auto &Blocks : Layout.StreamMap) {
    if (auto EC = DW.writeArray(Blocks))
      return std::move(EC);
  }

  return std::move(*OutOrError);
}

//===----------------------------------------------------------------------===//
// MSFOutputStream
//===----------------------------------------------------------------------===//

// Positional reads and writes, safe to call from several threads for
// different offsets of one file (unlike raw_fd_ostream::pwrite, which
// seeks). Both do all of `Len` bytes, or fail.
#if defined(_WIN32)
static std::error_code transferAt(int FD, void *Buf, size_t Len,
                                  uint64_t Offset, bool Write) {
  HANDLE H = reinterpret_cast<HANDLE>(_get_osfhandle(FD));
  while (Len) {
    OVERLAPPED O = {};
    O.Offset = static_cast<DWORD>(Offset);
    O.OffsetHigh = static_cast<DWORD>(Offset >> 32);
    DWORD Chunk = static_cast<DWORD>(std::min<size_t>(Len, 1u << 30));
    DWORD Done = 0;
    BOOL Ok = Write ? WriteFile(H, Buf, Chunk, &Done, &O)
                    : ReadFile(H, Buf, Chunk, &Done, &O);
    if (!Ok || Done == 0)
      return std::error_code(GetLastError(), std::system_category());
    Buf = static_cast<char *>(Buf) + Done;
    Len -= Done;
    Offset += Done;
  }
  return std::error_code();
}
#else
static std::error_code transferAt(int FD, void *Buf, size_t Len,
                                  uint64_t Offset, bool Write) {
  while (Len) {
    ssize_t N = Write ? ::pwrite(FD, Buf, Len, Offset)
                      : ::pread(FD, Buf, Len, Offset);
    if (N < 0) {
      if (errno == EINTR)
        continue;
      return errnoAsErrorCode();
    }
    if (N == 0)
      return std::make_error_code(std::errc::io_error);
    Buf = static_cast<char *>(Buf) + N;
    Len -= N;
    Offset += N;
  }
  return std::error_code();
}
#endif

static std::error_code writeAt(int FD, ArrayRef<uint8_t> Data,
                               uint64_t Offset) {
  return transferAt(FD, const_cast<uint8_t *>(Data.data()), Data.size(),
                    Offset, /*Write=*/true);
}

static std::error_code readAt(int FD, MutableArrayRef<uint8_t> Data,
                              uint64_t Offset) {
  return transferAt(FD, Data.data(), Data.size(), Offset, /*Write=*/false);
}

struct MSFOutputStream::WriteBuffer {
  uint64_t Offset = 0;
  std::vector<uint8_t> Data;
  // Up to where the buffer must not fill gaps: the end of what it held when
  // it last restarted behind it (see writeBytes()).
  uint64_t NoFillBelow = 0;
};

// Per-thread lookup of the thread's buffer of the stream last written on it,
// so that writeBytes() does not take the lock.
static thread_local uint64_t ThreadBufferOwner = 0;
static thread_local MSFOutputStream::WriteBuffer *ThreadBuffer = nullptr;
static std::atomic<uint64_t> NextStreamId{1};

static constexpr size_t WriteBufferCapacity = 1 << 20;

Expected<std::unique_ptr<MSFOutputStream>>
MSFOutputStream::create(StringRef Path, uint64_t Size, uint32_t BlockSize) {
  // The file is written next to its final path and renamed onto it in
  // commit(), like FileOutputBuffer does.
  Expected<sys::fs::TempFile> File = sys::fs::TempFile::create(
      Path + ".tmp%%%%%%%%", sys::fs::all_read | sys::fs::all_write);
  if (!File)
    return File.takeError();
  // Set the size up front: blocks that are never written stay zero, and the
  // file has its full size even if the last ones are among them.
  if (std::error_code EC = sys::fs::resize_file(File->FD, Size)) {
    consumeError(File->discard());
    return errorCodeToError(EC);
  }
  std::unique_ptr<MSFOutputStream> Stream(
      new MSFOutputStream(std::move(*File), Path.str(), Size, BlockSize));
  Stream->startWriteback();
  return Stream;
}

MSFOutputStream::MSFOutputStream(sys::fs::TempFile File, std::string Path,
                                 uint64_t Size, uint32_t BlockSize)
    : File(std::move(File)), Path(std::move(Path)), Size(Size),
      BlockSize(BlockSize), Id(NextStreamId++),
      BlockHashes((Size + BlockSize - 1) / BlockSize),
      BlockHashed((Size + BlockSize - 1) / BlockSize) {}

MSFOutputStream::~MSFOutputStream() {
  stopWriteback();
  // Not committed: nothing of the temporary file is wanted.
  if (File.FD != -1)
    consumeError(File.discard());
}

// On macOS, closing a file waits for its dirty pages to be written out,
// which for a big file that was just written takes a good part of a second
// on its own at the end. Have a thread ask for that every 50 ms while the
// file is being written, so that the writing out overlaps the writing
// and little is left at the end. (Elsewhere, close() does not wait, and the
// pages are written out in the background anyway.)
void MSFOutputStream::startWriteback() {
#ifdef __APPLE__
  WritebackThread = std::thread([this] {
    std::unique_lock<std::mutex> Lock(Mu);
    while (!WritebackCV.wait_for(Lock, std::chrono::milliseconds(50),
                                 [&] { return StopWriteback; })) {
      Lock.unlock();
      ::fsync(File.FD);
      Lock.lock();
    }
  });
#endif
}

void MSFOutputStream::stopWriteback() {
  if (!WritebackThread.joinable())
    return;
  {
    std::lock_guard<std::mutex> Lock(Mu);
    StopWriteback = true;
  }
  WritebackCV.notify_one();
  WritebackThread.join();
}

MSFOutputStream::WriteBuffer &MSFOutputStream::threadBuffer() {
  if (ThreadBufferOwner != Id) {
    std::lock_guard<std::mutex> Lock(Mu);
    Buffers.push_back(std::make_unique<WriteBuffer>());
    Buffers.back()->Data.reserve(WriteBufferCapacity);
    ThreadBuffer = Buffers.back().get();
    ThreadBufferOwner = Id;
  }
  return *ThreadBuffer;
}

// Writes out the buffer, whole blocks only unless `Final`: a partial last
// block stays, since the stream's next write continues it (and hashing a
// block wants all of it).
Error MSFOutputStream::flush(WriteBuffer &B, bool Final) {
  size_t Len = B.Data.size();
  if (!Final)
    Len -= Len % BlockSize;
  if (Len == 0)
    return Error::success();
  ArrayRef<uint8_t> Data(B.Data.data(), Len);
  std::error_code EC = writeAt(File.FD, Data, B.Offset);
  if (!EC)
    hashBlocks(B.Offset, Data);
  B.Data.erase(B.Data.begin(), B.Data.begin() + Len);
  B.Offset += Len;
  if (EC) {
    std::lock_guard<std::mutex> Lock(Mu);
    if (!FirstError)
      FirstError = EC;
    return errorCodeToError(EC);
  }
  return Error::success();
}

Error MSFOutputStream::flushAll() {
  std::lock_guard<std::mutex> Lock(Mu);
  for (std::unique_ptr<WriteBuffer> &B : Buffers)
    if (Error E = flush(*B, /*Final=*/true))
      consumeError(std::move(E)); // recorded in FirstError
  if (FirstError)
    return errorCodeToError(FirstError);
  return Error::success();
}

Error MSFOutputStream::writeBytes(uint64_t Offset, ArrayRef<uint8_t> Data) {
  if (Offset + Data.size() > Size)
    return make_error<BinaryStreamError>(stream_error_code::stream_too_short);
  WriteBuffer &B = threadBuffer();
  uint64_t End = B.Offset + B.Data.size();
  // A write back into what is buffered (a length fixed up after its contents
  // were written) is done in the buffer.
  if (!B.Data.empty() && Offset >= B.Offset && Offset + Data.size() <= End) {
    std::copy(Data.begin(), Data.end(), B.Data.begin() + (Offset - B.Offset));
    return Error::success();
  }
  // Consecutive writes are coalesced, and so are writes further on in the
  // same block: the gap is the unused tail of the previous stream's last
  // block, which nothing writes, so filling it in gives the same file. (A
  // gap that reaches into the next block could be a block of another stream,
  // e.g. the free page map's.) Except after a restart behind what was
  // written -- a fixup further back than the buffer -- where the "gap" up to
  // the old end holds data already written. Anything else flushes.
  bool FillGap = !B.Data.empty() && Offset > End &&
                 Offset <= alignTo(End, BlockSize) && End >= B.NoFillBelow;
  if (!B.Data.empty() && Offset != End && !FillGap) {
    if (Error E = flush(B, /*Final=*/true))
      return E;
    B.Data.clear();
    B.NoFillBelow = Offset < End ? End : 0;
  }
  if (B.Data.empty()) {
    B.Offset = Offset;
  } else if (FillGap) {
    // What an unused block tail holds: zero, except in the blocks at the
    // free page map's positions that the layout hands out as regular blocks
    // (a file's later intervals), which commitFpm() initialized to 0xFF.
    uint64_t Block = End / BlockSize;
    uint8_t Fill = Block % BlockSize == 1 || Block % BlockSize == 2 ? 0xFF : 0;
    B.Data.resize(Offset - B.Offset, Fill);
  }
  B.Data.insert(B.Data.end(), Data.begin(), Data.end());
  if (B.Data.size() >= WriteBufferCapacity)
    return flush(B, /*Final=*/false);
  return Error::success();
}

Error MSFOutputStream::readBytes(uint64_t Offset, uint64_t Len,
                                 ArrayRef<uint8_t> &Buffer) {
  // Reading back is not what this stream is for; it works, slowly.
  if (Offset + Len > Size)
    return make_error<BinaryStreamError>(stream_error_code::stream_too_short);
  if (Error E = flushAll())
    return E;
  static thread_local std::vector<uint8_t> Scratch;
  Scratch.resize(Len);
  if (std::error_code EC = readAt(File.FD, Scratch, Offset))
    return errorCodeToError(EC);
  Buffer = Scratch;
  return Error::success();
}

Error MSFOutputStream::readLongestContiguousChunk(uint64_t Offset,
                                                  ArrayRef<uint8_t> &Buffer) {
  if (Offset >= Size)
    return make_error<BinaryStreamError>(stream_error_code::stream_too_short);
  return readBytes(Offset, std::min<uint64_t>(Size - Offset, 1 << 16), Buffer);
}

// Hashes the blocks a flushed range covers (see hashContents()). Data is
// already in the file. A block only partially covered -- the range starts or
// ends inside it -- is hashed as zero-padded if this is the first of it, and
// re-read from the file and hashed whole if an earlier flush covered another
// part of it (a stream continued after a flush).
void MSFOutputStream::hashBlocks(uint64_t Offset, ArrayRef<uint8_t> Data) {
  uint64_t B = Offset / BlockSize;
  uint64_t Skip = Offset % BlockSize;
  auto hashFromFile = [&](uint64_t Block) {
    std::vector<uint8_t> Whole(BlockSize);
    // The file has its full size from the start, so this does not fail for
    // a reason an earlier write did not fail for.
    consumeError(errorCodeToError(readAt(File.FD, Whole, Block * BlockSize)));
    BlockHashes[Block] = xxh3_64bits(Whole);
    BlockHashed[Block] = true;
  };
  auto hashPartial = [&](uint64_t Block, ArrayRef<uint8_t> Part,
                         uint64_t At) {
    if (BlockHashed[Block])
      return hashFromFile(Block);
    std::vector<uint8_t> Padded(BlockSize, 0);
    std::copy(Part.begin(), Part.end(), Padded.begin() + At);
    BlockHashes[Block] = xxh3_64bits(Padded);
    BlockHashed[Block] = true;
  };
  if (Skip) {
    ArrayRef<uint8_t> Part = Data.take_front(BlockSize - Skip);
    hashPartial(B, Part, Skip);
    Data = Data.drop_front(Part.size());
    ++B;
  }
  for (; !Data.empty(); ++B) {
    ArrayRef<uint8_t> Block = Data.take_front(BlockSize);
    Data = Data.drop_front(Block.size());
    if (Block.size() < BlockSize) {
      hashPartial(B, Block, 0);
    } else if (BlockHashed[B]) {
      hashFromFile(B);
    } else {
      BlockHashes[B] = xxh3_64bits(Block);
      BlockHashed[B] = true;
    }
  }
}

Expected<uint64_t> MSFOutputStream::hashContents() {
  if (Error E = flushAll())
    return std::move(E);
  // Blocks nothing wrote are zero.
  uint64_t ZeroHash = xxh3_64bits(std::vector<uint8_t>(BlockSize, 0));
  for (size_t B = 0, E = BlockHashes.size(); B != E; ++B)
    if (!BlockHashed[B])
      BlockHashes[B] = ZeroHash;
  return xxh3_64bits(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(BlockHashes.data()),
      BlockHashes.size() * sizeof(uint64_t)));
}

Error MSFOutputStream::commit() {
  if (Error E = flushAll())
    return E;
  stopWriteback();
  // Renames the file onto the final path and closes it.
  return File.keep(Path);
}
