//===- dsymutil2.cpp - A fast dSYM linker ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// dsymutil2 links the DWARF in the object files of a Mach-O binary's debug map
// into a .dSYM bundle, like dsymutil. It is an experiment in doing that fast:
// it is a short sequence of passes, each a parallel loop over the object
// files, and it copies and patches DWARF instead of parsing and regenerating
// it.
//
//===----------------------------------------------------------------------===//

#include "dsymutil2.h"
#include "DebugMap.h"
#include "DsymWriter.h"
#include "MachOFile.h"
#include "ObjectFile.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::dsymutil2;

namespace {
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

using namespace llvm::opt;
#define OPTTABLE_CODE
#include "Options.inc"

class Dsymutil2OptTable : public opt::OptTable {
public:
  Dsymutil2OptTable() : opt::OptTable(optionTables()) {}
};

struct Options {
  std::string InputFile;
  /// The dSYM bundle.
  std::string OutputFile;
  std::string OsoPrependPath;
  unsigned NumThreads = 0;
  bool DumpDebugMap = false;
};
} // namespace

static bool TimePhases = false;

PhaseTimer::PhaseTimer(const char *Name)
    : Name(Name), Start(std::chrono::steady_clock::now()) {}

PhaseTimer::~PhaseTimer() {
  if (!TimePhases)
    return;
  std::chrono::duration<double, std::milli> Elapsed =
      std::chrono::steady_clock::now() - Start;
  errs() << format("%-24s %8.1f ms\n", Name, Elapsed.count());
}

void dsymutil2::fatal(const Twine &Message) {
  WithColor::error(errs(), "dsymutil2") << Message << '\n';
  exit(1);
}

void dsymutil2::warn(const Twine &Message) {
  WithColor::warning(errs(), "dsymutil2") << Message << '\n';
}

static void run(const Options &Opts) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrError =
      MemoryBuffer::getFile(Opts.InputFile, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (std::error_code EC = BufferOrError.getError())
    fatal(Opts.InputFile + ": " + EC.message());
  // Deliberately leaked, like everything else that maps files: see main().
  MemoryBuffer *BinaryBuffer = BufferOrError->release();
  MachOFile Binary;
  std::string Error = Binary.parse(BinaryBuffer->getBuffer());
  if (!Error.empty())
    fatal(Opts.InputFile + ": " + Error);

  DebugMap Map;
  {
    PhaseTimer Timer("read debug map");
    Map = readDebugMap(Binary);
  }
  InputFiles Inputs = openObjectFiles(Map, Opts.OsoPrependPath);
  for (const ObjectFile &Object : Inputs.Objects)
    if (!Object.Warning.empty())
      warn(Object.Map->Path + ": " + Object.Warning);
  if (Opts.DumpDebugMap) {
    dumpDebugMap(Inputs.Objects, outs());
    return;
  }

  DsymWriter Writer(Binary);
  {
    PhaseTimer Timer("create output");
    Writer.create(Opts.OutputFile, sys::path::filename(Opts.InputFile));
  }
  {
    PhaseTimer Timer("commit output");
    Writer.commit();
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  Dsymutil2OptTable T;
  unsigned MissingIndex, MissingCount;
  opt::InputArgList Args =
      T.ParseArgs(ArrayRef(argv + 1, argc - 1), MissingIndex, MissingCount);
  if (MissingCount)
    fatal(Twine("missing argument to ") + Args.getArgString(MissingIndex));
  for (const opt::Arg *Arg : Args.filtered(OPT_UNKNOWN))
    fatal("unknown option: " + Arg->getSpelling());

  if (Args.hasArg(OPT_help)) {
    T.printHelp(outs(), "dsymutil2 [options] <linked binary>",
                "fast dSYM linker", /*ShowHidden=*/false);
    return 0;
  }
  if (Args.hasArg(OPT_version)) {
    cl::PrintVersionMessage();
    return 0;
  }

  Options Opts;
  std::vector<std::string> InputFiles = Args.getAllArgValues(OPT_INPUT);
  if (InputFiles.size() != 1)
    fatal("expected exactly one input file");
  Opts.InputFile = InputFiles[0];
  Opts.OutputFile =
      Args.getLastArgValue(OPT_output, Opts.InputFile + ".dSYM").str();
  Opts.OsoPrependPath = Args.getLastArgValue(OPT_oso_prepend_path).str();
  Opts.DumpDebugMap = Args.hasArg(OPT_dump_debug_map);
  TimePhases = Args.hasArg(OPT_time_phases);
  if (const opt::Arg *Threads = Args.getLastArg(OPT_threads))
    if (StringRef(Threads->getValue()).getAsInteger(10, Opts.NumThreads))
      fatal(Twine("invalid number of threads: ") + Threads->getValue());
  parallel::strategy = hardware_concurrency(Opts.NumThreads);

  {
    PhaseTimer Timer("total");
    run(Opts);
  }
  // Unmapping tens of thousands of files takes a while and gains nothing.
  outs().flush();
  errs().flush();
  sys::Process::Exit(0, /*NoCleanup=*/true);
}
