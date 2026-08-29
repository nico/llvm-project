//===- Driver.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_DRIVER_H
#define LLD_COFF_DRIVER_H

#include "Config.h"
#include "SymbolTable.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Reproduce.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TarWriter.h"
#include "llvm/WindowsDriver/MSVCPaths.h"
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace lld::coff {

using llvm::COFF::MachineTypes;
using llvm::COFF::WindowsSubsystem;
using std::optional;

class COFFOptTable : public llvm::opt::GenericOptTable {
public:
  COFFOptTable();
};

// The result of parsing the .drective section. The /export: and /include:
// options are handled separately because they reference symbols, and the number
// of symbols can be quite large. The LLVM Option library will perform at least
// one memory allocation per argument, and that is prohibitively slow for
// parsing directives.
class ArgParser {
public:
  ArgParser(COFFLinkerContext &ctx);

  // Parses command line options.
  llvm::opt::InputArgList parse(llvm::ArrayRef<const char *> args);

  // Tokenizes a given string and then parses as command line options.
  llvm::opt::InputArgList parse(StringRef s) { return parse(tokenize(s)); }

  // Tokenizes a given string and then parses as command line options in
  // .drectve section. /EXPORT options are returned in second element
  // to be processed in fastpath. Strings that need copying go into `saver`
  // (this may run on a worker thread, see perThreadSaver()).
  ParsedDirectives parseDirectives(StringRef s, llvm::StringSaver &saver);

private:
  // Concatenate LINK environment variable.
  void addLINK(SmallVector<const char *, 256> &argv);

  std::vector<const char *> tokenize(StringRef s);

  COFFLinkerContext &ctx;
};

// A string saver for code that runs on worker threads; lld's saver() is not
// thread-safe. The strings live as long as saver()'s.
llvm::StringSaver &perThreadSaver();

class InputFileReader;
struct InputFileRequest;

class LinkerDriver {
public:
  LinkerDriver(COFFLinkerContext &ctx);
  ~LinkerDriver();

  void linkerMain(llvm::ArrayRef<const char *> args);

  void addFile(InputFile *file);

  void addClangLibSearchPaths(const std::string &argv0);

  // Used by ArchiveFile to enqueue members.
  void enqueueArchiveMember(const Archive::Child &c, const Archive::Symbol &sym,
                            StringRef parentName);

  enum class InputOpt { None, DefaultLib, WholeArchive };
  void enqueuePDB(StringRef Path) { enqueuePath(Path, false); }

  MemoryBufferRef takeBuffer(std::unique_ptr<MemoryBuffer> mb);

  void enqueuePath(StringRef path, bool lazy,
                   InputOpt inputOpt = InputOpt::None);

  // Returns a list of chunks of selected symbols.
  std::vector<Chunk *> getChunks() const;

  std::unique_ptr<llvm::TarWriter> tar; // for /linkrepro

  void pullArm64ECIcallHelper();

private:
  // Searches a file from search paths.
  std::optional<StringRef> findFileIfNew(StringRef filename);
  std::optional<StringRef> findLibIfNew(StringRef filename);
  StringRef findFile(StringRef filename);
  StringRef findLib(StringRef filename);
  StringRef findLibMinGW(StringRef filename);

  // Determines the location of the sysroot based on `args`, environment, etc.
  void detectWinSysRoot(const llvm::opt::InputArgList &args);

  // Adds various search paths based on the sysroot.  Must only be called once
  // config.machine has been set.
  void addWinSysRootLibSearchPaths();

  void setMachine(llvm::COFF::MachineTypes machine);
  llvm::Triple::ArchType getArch();

  uint64_t getDefaultImageBase();

  bool isDecorated(StringRef sym);

  InputFile *addObjectFile(COFFLinkerContext &ctx, MemoryBufferRef mb,
                           StringRef archiveName, uint64_t offsetInArchive,
                           bool lazy);

  std::string getMapFile(const llvm::opt::InputArgList &args,
                         llvm::opt::OptSpecifier os,
                         llvm::opt::OptSpecifier osFile);

  std::string getImplibPath();

  // The import name is calculated as follows:
  //
  //        | LIBRARY w/ ext |   LIBRARY w/o ext   | no LIBRARY
  //   -----+----------------+---------------------+------------------
  //   LINK | {value}        | {value}.{.dll/.exe} | {output name}
  //    LIB | {value}        | {value}.dll         | {output name}.dll
  //
  std::string getImportName(bool asLib);

  // Write fullly resolved path to repro file if /linkreprofullpathrsp
  // is specified.
  void handleReproFile(StringRef path, InputOpt inputOpt);

  void createImportLibrary(bool asLib);

  // Used by the resolver to parse .drectve section contents. `rootsDone`: the
  // /include and /entry roots were already added (as events of the sharded
  // replay).
  void parseDirectives(InputFile *file, bool rootsDone = false);

  // Parse an /order file. If an option is given, the linker places COMDAT
  // sections int he same order as their names appear in the given file.
  void parseOrderFile(StringRef arg);

  void parseCallGraphFile(StringRef path);

  void parsePDBAltPath();

  // Parses LIB environment which contains a list of search paths.
  void addLibSearchPaths();

  // Library search path. The first element is always "" (current directory).
  std::vector<StringRef> searchPaths;

  // Convert resource files and potentially merge input resource object
  // trees into one resource tree.
  void convertResources();

  void maybeExportMinGWSymbols(const llvm::opt::InputArgList &args);

  // We don't want to add the same file more than once.
  // Files are uniquified by their filesystem and file number.
  std::set<llvm::sys::fs::UniqueID> visitedFiles;

  std::set<std::string> visitedLibs;

  void addBuffer(std::unique_ptr<MemoryBuffer> mb, bool wholeArchive,
                 bool lazy);
  void addArchiveBuffer(MemoryBufferRef mbref, StringRef symName,
                        StringRef parentName, uint64_t offsetInArchive,
                        bool lazy);
  void addThinArchiveBuffer(MemoryBufferRef mbref, StringRef symName,
                            bool lazy);

  // Queues a task for run(). `request` is the input file the task takes from
  // the reader, if any; run() uses it to see whether the task would wait.
  void enqueueTask(std::function<void()> task,
                   std::shared_ptr<InputFileRequest> request = nullptr);
  bool run();

  struct Task {
    std::function<void()> fn;
    std::shared_ptr<InputFileRequest> request;
  };
  std::list<Task> taskQueue;

  // While run() runs a batch of tasks, addFile() only collects the files here;
  // they are added once the batch's object files have been prepared. An entry
  // with a null file is something to do at that position instead, as the
  // tasks did when each ran to completion in turn: the retry of a file that
  // could not be opened (it needs the search paths the files before it add),
  // or a log message that follows the file's "Reading" one.
  struct BatchEntry {
    InputFile *file;
    std::function<void()> deferred;
  };
  bool collectingBatch = false;
  std::vector<BatchEntry> batch;

  // While run() adds a batch's files, addFile() only does the symbol table
  // half of an object file's parsing and collects the file here; the rest is
  // done for all of them at once by finishBatch(), in parallel.
  bool addingBatch = false;
  std::vector<ObjFile *> pendingFinish;
  void finishBatch();

  // Adds a run of files whose symbol table halves can be done with one thread
  // per shard of the symbol table (InputFile::shardable()), see Driver.cpp.
  // `firstIndex` is the position of the first one in the batch, for the
  // ordering keys.
  void addSegment(ArrayRef<InputFile *> files, uint32_t firstIndex);
  // The machine-type check of addFile(); false on a mismatch.
  bool checkMachineType(InputFile *file);
  // The part of addFile() after the file's symbols are in.
  void addFileRest(InputFile *file, bool rootsDone);

  // While a segment is being added, tasks are not queued right away but
  // recorded with the key of what queued them, and queued in key order at
  // the end, since the segment's events run out of input order.
  bool recordingTasks = false;
  struct RecordedTask {
    uint64_t key;
    Task task;
  };
  std::vector<RecordedTask> recordedTasks;

  // Opens input files ahead of the tasks that parse them, see Driver.cpp.
  std::unique_ptr<InputFileReader> reader;
  std::vector<MemoryBufferRef> resources;

  llvm::DenseSet<StringRef> excludedSymbols;

  COFFLinkerContext &ctx;

  llvm::ToolsetLayout vsLayout = llvm::ToolsetLayout::OlderVS;
  std::string vcToolChainPath;
  llvm::SmallString<128> diaPath;
  bool useWinSysRootLibPath = false;
  llvm::SmallString<128> universalCRTLibPath;
  int sdkMajor = 0;
  llvm::SmallString<128> windowsSdkLibPath;

  // For linkreprofullpathrsp
  std::unique_ptr<llvm::raw_fd_ostream> reproFile;

  // Functions below this line are defined in DriverUtils.cpp.

  void printHelp(const char *argv0);

  // Parses a string in the form of "<integer>[,<integer>]".
  void parseNumbers(StringRef arg, uint64_t *addr, uint64_t *size = nullptr);

  void parseGuard(StringRef arg);

  // Parses a string in the form of "<integer>[.<integer>]".
  // Minor's default value is 0.
  void parseVersion(StringRef arg, uint32_t *major, uint32_t *minor);

  // Parses a string in the form of "<subsystem>[,<integer>[.<integer>]]".
  void parseSubsystem(StringRef arg, WindowsSubsystem *sys, uint32_t *major,
                      uint32_t *minor, bool *gotVersion = nullptr);

  void parseMerge(StringRef);
  void parsePDBPageSize(StringRef);
  void parseSection(StringRef);
  void parseSectionLayout(StringRef);

  void parseSameAddress(StringRef);

  // Parses a MS-DOS stub file
  void parseDosStub(StringRef path);

  // Parses a string in the form of "[:<integer>]"
  void parseFunctionPadMin(llvm::opt::Arg *a);

  // Parses a string in the form of "[:<integer>]"
  void parseDependentLoadFlags(llvm::opt::Arg *a);

  // Parses a string in the form of "EMBED[,=<integer>]|NO".
  void parseManifest(StringRef arg);

  // Parses a string in the form of "level=<string>|uiAccess=<string>"
  void parseManifestUAC(StringRef arg);

  // Parses a string in the form of "cd|net[,(cd|net)]*"
  void parseSwaprun(StringRef arg);

  // Create a resource file containing a manifest XML.
  std::unique_ptr<MemoryBuffer> createManifestRes();
  void createSideBySideManifest();
  std::string createDefaultXml();
  std::string createManifestXmlWithInternalMt(StringRef defaultXml);
  std::string createManifestXmlWithExternalMt(StringRef defaultXml);
  std::string createManifestXml();

  std::unique_ptr<llvm::WritableMemoryBuffer>
  createMemoryBufferForManifestRes(size_t manifestRes);

  // Used for dllexported symbols.
  Export parseExport(StringRef arg);

  // Parses a string in the form of "key=value" and check
  // if value matches previous values for the key.
  // This feature used in the directive section to reject
  // incompatible objects.
  void checkFailIfMismatch(StringRef arg, InputFile *source);

  // Convert Windows resource files (.res files) to a .obj file.
  MemoryBufferRef convertResToCOFF(ArrayRef<MemoryBufferRef> mbs,
                                   ArrayRef<ObjFile *> objs);

  // Create export thunks for exported and patchable Arm64EC function symbols.
  void createECExportThunks();
  void maybeCreateECExportThunk(StringRef name, Symbol *&sym);

  bool ltoCompilationDone = false;
};

// Create enum with OPT_xxx values for each option in Options.td
enum {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

} // namespace lld::coff

#endif
