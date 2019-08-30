#include "Writer.h"
#include "Config.h"
#include "InputSection.h"
#include "InputFiles.h"
#include "OutputSegment.h"
#include "SymbolTable.h"
#include "Symbols.h"

#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"

using namespace lld;
using namespace lld::macho;
using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::support;

namespace {
class LCHeader;
class LCLinkEdit;
class LCDyldInfo;
class LCSymtab;

class LoadCommand {
public:
  virtual ~LoadCommand() {}
  virtual uint64_t getSize() = 0;
  virtual void writeTo(uint8_t *Buf) = 0;
};

class Writer {
public:
  Writer() : Buffer(errorHandler().OutputBuffer) {}

  std::vector<LoadCommand *> LoadCommands;

  std::unique_ptr<FileOutputBuffer> &Buffer;
  uint64_t FileSize;

  void createLoadCommands();
  void assignAddresses();

  void createDyldInfoContents();
  void createSymtabContents();

  void openFile();
  void writeHeader();
  void writeSections();

  void run();

  LCHeader *HeaderSeg = nullptr;
  LCLinkEdit *LinkEditSeg = nullptr;
  LCDyldInfo *DyldInfoSeg = nullptr;
  LCSymtab *SymtabSeg = nullptr;
};

class LCPagezero : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(segment_command_64); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<segment_command_64 *>(Buf);
    C->cmd = LC_SEGMENT_64;
    C->cmdsize = getSize();
    strcpy(C->segname, "__PAGEZERO");
    C->vmsize = PageSize;
  }
};

class LCLinkEdit : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(segment_command_64); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<segment_command_64 *>(Buf);
    C->cmd = LC_SEGMENT_64;
    C->cmdsize = getSize();
    strcpy(C->segname, "__LINKEDIT");
    C->fileoff = FileOff;
    C->filesize = Contents.size();
    C->maxprot = VM_PROT_READ | VM_PROT_WRITE;
    C->initprot = VM_PROT_READ;
  }

  uint64_t getOffset() { return FileOff + Contents.size(); }

  uint64_t FileOff = 0;

  SmallVector<char, 128> Contents;
  raw_svector_ostream OS{Contents};
};

class LCHeader : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(segment_command_64); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<segment_command_64 *>(Buf);

    C->cmd = LC_SEGMENT_64;
    C->cmdsize = getSize();
    strcpy(C->segname, "__HEADER");
    C->vmaddr = ImageBase;
    C->vmsize = alignTo(sizeof(mach_header_64) + SizeofCmds, PageSize);
    C->filesize = C->vmsize;
    C->maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    C->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  }

  uint64_t SizeofCmds;
};

class LCDyldInfo : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(dyld_info_command); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<dyld_info_command *>(Buf);
    C->cmd = LC_DYLD_INFO_ONLY;
    C->cmdsize = getSize();
    C->export_off = ExportOff;
    C->export_size = ExportSize;
  }

  uint64_t ExportOff = 0;
  uint64_t ExportSize = 0;
};

class LCDysymtab : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(dysymtab_command); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<dysymtab_command *>(Buf);
    C->cmd = LC_DYSYMTAB;
    C->cmdsize = getSize();
  }
};

class LCSegment : public LoadCommand {
public:
  LCSegment(StringRef Name, OutputSegment *Seg) : Name(Name), Seg(Seg) {}

  uint64_t getSize() {
    return sizeof(segment_command_64) +
           Seg->Sections.size() * sizeof(section_64);
  }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<segment_command_64 *>(Buf);
    Buf += sizeof(segment_command_64);

    C->cmd = LC_SEGMENT_64;
    C->cmdsize = getSize();
    memcpy(C->segname, Name.data(), Name.size());

    InputSection *FirstSec = Seg->Sections.front().second[0];
    InputSection *LastSec = Seg->Sections.back().second.back();

    C->vmaddr = FirstSec->Addr;
    C->fileoff = FirstSec->Addr - ImageBase;
    C->vmsize = C->filesize =
        LastSec->Addr + LastSec->Data.size() - FirstSec->Addr;
    C->maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    C->initprot = Seg->Perms;
    C->nsects = Seg->Sections.size();

    for (auto &P : Seg->Sections) {
      StringRef S = P.first;
      std::vector<InputSection *> &Sections = P.second;

      auto *SectHdr = reinterpret_cast<section_64 *>(Buf);
      Buf += sizeof(section_64);

      memcpy(SectHdr->sectname, S.data(), S.size());
      memcpy(SectHdr->segname, Name.data(), Name.size());

      SectHdr->addr = Sections[0]->Addr;
      SectHdr->offset = Sections[0]->Addr - ImageBase;
      SectHdr->size = Sections.back()->Addr + Sections.back()->Data.size() -
                      Sections[0]->Addr;
    }
  }

private:
  StringRef Name;
  OutputSegment *Seg;
};

class LCMain : public LoadCommand {
  uint64_t getSize() { return sizeof(entry_point_command); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<entry_point_command *>(Buf);
    C->cmd = LC_MAIN;
    C->cmdsize = getSize();
    C->entryoff = Config->Entry->getVA();
    C->stacksize = 0;
  }
};

class LCSymtab : public LoadCommand {
public:
  uint64_t getSize() { return sizeof(symtab_command); }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<symtab_command *>(Buf);
    C->cmd = LC_SYMTAB;
    C->cmdsize = getSize();
    C->symoff = SymOff;
    C->nsyms = NSyms;
    C->stroff = StrOff;
    C->strsize = StrSize;
  }

  uint64_t SymOff = 0;
  uint64_t NSyms = 0;
  uint64_t StrOff = 0;
  uint64_t StrSize = 0;
};

class LCLoadDylib : public LoadCommand {
public:
  LCLoadDylib(StringRef Path) : Path(Path) {}

  uint64_t getSize() {
    return alignTo(sizeof(dylib_command) + Path.size() + 1, 8);
  }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<dylib_command *>(Buf);
    Buf += sizeof(dylib_command);

    C->cmd = LC_LOAD_DYLIB;
    C->cmdsize = getSize();
    C->dylib.name = sizeof(dylib_command);

    memcpy(Buf, Path.data(), Path.size());
    Buf[Path.size()] = '\0';
  }

private:
  StringRef Path;
};

class LCLoadDylinker : public LoadCommand {
public:
  uint64_t getSize() {
    return alignTo(sizeof(dylinker_command) + Path.size() + 1, 8);
  }

  void writeTo(uint8_t *Buf) {
    auto *C = reinterpret_cast<dylinker_command *>(Buf);
    Buf += sizeof(dylinker_command);

    C->cmd = LC_LOAD_DYLINKER;
    C->cmdsize = getSize();
    C->name = sizeof(dylinker_command);

    memcpy(Buf, Path.data(), Path.size());
    Buf[Path.size()] = '\0';
  }

private:
  StringRef Path = "/usr/lib/dyld";
};

int numDylibs() {
  int NumDylibs = 0;
  for (InputFile *File : InputFiles)
    if (!File->DylibName.empty())
      ++NumDylibs;
  return NumDylibs;
}
} // namespace

void Writer::createLoadCommands() {
  HeaderSeg = make<LCHeader>();
  LinkEditSeg = make<LCLinkEdit>();
  DyldInfoSeg = make<LCDyldInfo>();
  SymtabSeg = make<LCSymtab>();

  LoadCommands.push_back(HeaderSeg);
  LoadCommands.push_back(make<LCPagezero>());

  for (OutputSegment *Seg : OutputSegments)
    if (!Seg->Sections.empty())
      LoadCommands.push_back(make<LCSegment>(Seg->Name, Seg));

  LoadCommands.push_back(LinkEditSeg);
  LoadCommands.push_back(DyldInfoSeg);
  LoadCommands.push_back(SymtabSeg);
  LoadCommands.push_back(make<LCDysymtab>());
  LoadCommands.push_back(make<LCLoadDylinker>());

  if (numDylibs() == 0)  // LC_MAIN needs load command for libdyld.dylib.
    error("dynamic main executables must link with libSystem.dylib");
  LoadCommands.push_back(make<LCMain>());

  for (InputFile *File : InputFiles)
    if (!File->DylibName.empty())
      LoadCommands.push_back(make<LCLoadDylib>(File->DylibName));
}

void Writer::assignAddresses() {
  uint64_t Addr = ImageBase + sizeof(mach_header_64);

  uint64_t Size = 0;
  for (LoadCommand *LC : LoadCommands)
    Size += LC->getSize();
  HeaderSeg->SizeofCmds = Size;
  Addr += Size;

  for (OutputSegment *Seg : OutputSegments) {
    Addr = alignTo(Addr, PageSize);

    for (auto &P : Seg->Sections) {
      ArrayRef<InputSection *> Sections = P.second;
      for (InputSection *IS : Sections) {
        Addr = alignTo(Addr, IS->Align);
        IS->Addr = Addr;
        Addr += IS->Data.size();
      }
    }
  }

  LinkEditSeg->FileOff = alignTo(Addr - ImageBase, sizeof(void*));
}

void Writer::createDyldInfoContents() {
  uint64_t Start = LinkEditSeg->getOffset();
  raw_svector_ostream &OS = LinkEditSeg->OS;

  // Build an export symbol trie that contains only `_main`.
  StringRef SymName = Config->Entry->getName();
  uint64_t Addr = Config->Entry->getVA();

  OS << (char)0;  // Indicates non-leaf node
  OS << (char)1;  // # of child -- we only have `_main`
  OS << SymName << '\0';
  encodeULEB128(SymName.size() + 4, OS); // Leaf offset

  // Leaf node
  OS << (char)(1 + getULEB128Size(Addr)); // Node length
  OS << (char)0;			  // Flags
  encodeULEB128(Addr, OS);		  // Address
  OS << (char)0;			  // Terminator

  DyldInfoSeg->ExportOff = Start;
  DyldInfoSeg->ExportSize = LinkEditSeg->getOffset() - Start;

  int pad = alignTo(LinkEditSeg->Contents.size(), sizeof(void *)) -
            LinkEditSeg->Contents.size();
  for (int i = 0; i < pad; ++i)
    OS << (char)0;
}

void Writer::createSymtabContents() {
  uint64_t Start = LinkEditSeg->getOffset();

  // nlist N;
  // N->n_strx = strOffset - _startOfSymbolStrings;
  // N->n_type = sym.type | sym.scope;
  // N->n_sect = sym.sect;
  // N->n_desc = sym.desc;
  // N->n_value = sym.value;

  //  for (Symbol *Sym : Symtab->getSymbols())
  //    if (isa<Defined>(Sym))
  //      outs() << "Sym=" << Sym->getName() << " 0x" << Sym->getVA() << "\n";

  SymtabSeg->SymOff = Start;
  SymtabSeg->NSyms = 0;
  SymtabSeg->StrOff = Start;
  SymtabSeg->StrSize = 0;
}

void Writer::openFile() {
  Expected<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(Config->OutputFile, FileSize,
                               FileOutputBuffer::F_executable);

  if (!BufferOrErr)
    error("failed to open " + Config->OutputFile + ": " +
          llvm::toString(BufferOrErr.takeError()));
  else
    Buffer = std::move(*BufferOrErr);
}

void Writer::writeHeader() {
  auto *Hdr = reinterpret_cast<mach_header_64 *>(Buffer->getBufferStart());
  Hdr->magic = MH_MAGIC_64;
  Hdr->cputype = CPU_TYPE_X86_64;
  Hdr->cpusubtype = CPU_SUBTYPE_X86_64_ALL | CPU_SUBTYPE_LIB64;
  Hdr->filetype = MH_EXECUTE;
  Hdr->ncmds = LoadCommands.size();
  Hdr->sizeofcmds = HeaderSeg->SizeofCmds;
  Hdr->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

  uint8_t *P = reinterpret_cast<uint8_t *>(Hdr + 1);
  for (LoadCommand *LC : LoadCommands) {
    LC->writeTo(P);
    P += LC->getSize();
  }
}

void Writer::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();

  for (OutputSegment *Seg : OutputSegments)
    for (auto &Sect : Seg->Sections)
      for (InputSection *IS : Sect.second)
        IS->writeTo(Buf + IS->Addr - ImageBase);

  memcpy(Buf + LinkEditSeg->FileOff, LinkEditSeg->Contents.data(),
	 LinkEditSeg->Contents.size());
}

void Writer::run() {
  createLoadCommands();
  assignAddresses();

  // Fill __LINKEDIT contents
  createDyldInfoContents();
  createSymtabContents();
  FileSize = LinkEditSeg->FileOff + LinkEditSeg->Contents.size();

  openFile();
  if (errorCount())
    return;

  writeHeader();
  writeSections();

  if (auto E = Buffer->commit())
    error("failed to write to the output file: " + toString(std::move(E)));
}

void macho::writeResult() { Writer().run(); }
