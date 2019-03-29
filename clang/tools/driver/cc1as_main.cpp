//===-- cc1as_main.cpp - Clang Assembler  ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the entry point to the clang -cc1as functionality, which implements
// the direct interface to the LLVM MC based assembler.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <system_error>
using namespace clang;
using namespace clang::driver;
using namespace clang::driver::options;
using namespace llvm;
using namespace llvm::opt;

namespace {

/// Helper class for representing a single invocation of the assembler.
struct AssemblerInvocation {
  /// @name Target Options
  /// @{

  /// The name of the target triple to assemble for.
  std::string Triple;

  /// If given, the name of the target CPU to determine which instructions
  /// are legal.
  std::string CPU;

  /// The list of target specific features to enable or disable -- this should
  /// be a list of strings starting with '+' or '-'.
  std::vector<std::string> Features;

  /// The list of symbol definitions.
  std::vector<std::string> SymbolDefs;

  /// @}
  /// @name Language Options
  /// @{

  std::vector<std::string> IncludePaths;
  unsigned NoInitialTextSection : 1;
  unsigned SaveTemporaryLabels : 1;
  unsigned GenDwarfForAssembly : 1;
  unsigned RelaxELFRelocations : 1;
  unsigned DwarfVersion;
  std::string DwarfDebugFlags;
  std::string DwarfDebugProducer;
  std::string DebugCompilationDir;
  std::map<const std::string, const std::string> DebugPrefixMap;
  llvm::DebugCompressionType CompressDebugSections =
      llvm::DebugCompressionType::None;
  std::string MainFileName;
  std::string SplitDwarfFile;

  /// @}
  /// @name Frontend Options
  /// @{

  std::string InputFile;
  std::vector<std::string> LLVMArgs;
  std::string OutputPath;
  enum FileType {
    FT_Asm,  ///< Assembly (.s) output, transliterate mode.
    FT_Null, ///< No output, for timing purposes.
    FT_Obj   ///< Object file output.
  };
  FileType OutputType;
  unsigned ShowHelp : 1;
  unsigned ShowVersion : 1;

  /// @}
  /// @name Transliterate Options
  /// @{

  unsigned OutputAsmVariant;
  unsigned ShowEncoding : 1;
  unsigned ShowInst : 1;

  /// @}
  /// @name Assembler Options
  /// @{

  unsigned RelaxAll : 1;
  unsigned NoExecStack : 1;
  unsigned FatalWarnings : 1;
  unsigned IncrementalLinkerCompatible : 1;
  unsigned EmbedBitcode : 1;

  /// The name of the relocation model to use.
  std::string RelocationModel;

  /// The ABI targeted by the backend. Specified using -target-abi. Empty
  /// otherwise.
  std::string TargetABI;

  /// @}

public:
  AssemblerInvocation() {
    Triple = "";
    NoInitialTextSection = 0;
    InputFile = "-";
    OutputPath = "-";
    OutputType = FT_Asm;
    OutputAsmVariant = 0;
    ShowInst = 0;
    ShowEncoding = 0;
    RelaxAll = 0;
    NoExecStack = 0;
    FatalWarnings = 0;
    IncrementalLinkerCompatible = 0;
    DwarfVersion = 0;
    EmbedBitcode = 0;
  }

  static bool CreateFromArgs(AssemblerInvocation &Res,
                             ArrayRef<const char *> Argv,
                             DiagnosticsEngine &Diags, LangOptions &LO);
};

} // namespace


// FIXME: dedupe with driver.cpp
// This lets us create the DiagnosticsEngine with a properly-filled-out
// DiagnosticOptions instance.
static DiagnosticOptions *
CreateAndPopulateDiagOpts(ArrayRef<const char *> argv) {
  auto *DiagOpts = new DiagnosticOptions;
  std::unique_ptr<OptTable> Opts(createDriverOptTable());
  unsigned MissingArgIndex, MissingArgCount;
  InputArgList Args =
      Opts->ParseArgs(argv.slice(1), MissingArgIndex, MissingArgCount);
  // We ignore MissingArgCount and the return value of ParseDiagnosticArgs.
  // Any errors that would be diagnosed here will also be diagnosed later,
  // when the DiagnosticsEngine actually exists.
  (void)ParseDiagnosticArgs(*DiagOpts, Args, nullptr,
                            /*DefaultDiagColor=*/false);
  return DiagOpts;
}

bool AssemblerInvocation::CreateFromArgs(AssemblerInvocation &Opts,
                                         ArrayRef<const char *> Argv,
                                         DiagnosticsEngine &Diags,
                                         LangOptions &LO) {
  bool Success = true;

  // Parse the arguments.
  std::unique_ptr<OptTable> OptTbl(createDriverOptTable());

  const unsigned IncludedFlagsBitmask = options::CC1AsOption;
  unsigned MissingArgIndex, MissingArgCount;
  InputArgList Args = OptTbl->ParseArgs(Argv, MissingArgIndex, MissingArgCount,
                                        IncludedFlagsBitmask);

  LO.MSCompatibilityVersion = 0;
// FIXME: OPT_fms_compatibility_version doesn't work for some reason
// Probably because ClangAs::ContructJob() passes a nullptr TC.
  if (const Arg *A = Args.getLastArg(OPT_fms_compatibility_version)) {
    VersionTuple VT;
    if (VT.tryParse(A->getValue()))
      Diags.Report(diag::err_drv_invalid_value) << A->getAsString(Args)
                                                << A->getValue();
    LO.MSCompatibilityVersion = VT.getMajor() * 10000000 +
                                VT.getMinor().getValueOr(0) * 100000 +
                                VT.getSubminor().getValueOr(0);
  }

  // Check for missing argument error.
  if (MissingArgCount) {
    Diags.Report(diag::err_drv_missing_argument)
        << Args.getArgString(MissingArgIndex) << MissingArgCount;
    Success = false;
  }

  // Issue errors on unknown arguments.
  for (const Arg *A : Args.filtered(OPT_UNKNOWN)) {
    auto ArgString = A->getAsString(Args);
    std::string Nearest;
    if (OptTbl->findNearest(ArgString, Nearest, IncludedFlagsBitmask) > 1)
      Diags.Report(diag::err_drv_unknown_argument) << ArgString;
    else
      Diags.Report(diag::err_drv_unknown_argument_with_suggestion)
          << ArgString << Nearest;
    Success = false;
  }

  // Construct the invocation.

  // Target Options
  Opts.Triple = llvm::Triple::normalize(Args.getLastArgValue(OPT_triple));
  Opts.CPU = Args.getLastArgValue(OPT_target_cpu);
  Opts.Features = Args.getAllArgValues(OPT_target_feature);

  // Use the default target triple if unspecified.
  if (Opts.Triple.empty())
    Opts.Triple = llvm::sys::getDefaultTargetTriple();

  // Language Options
  Opts.IncludePaths = Args.getAllArgValues(OPT_I);
  Opts.NoInitialTextSection = Args.hasArg(OPT_n);
  Opts.SaveTemporaryLabels = Args.hasArg(OPT_msave_temp_labels);
  // Any DebugInfoKind implies GenDwarfForAssembly.
  Opts.GenDwarfForAssembly = Args.hasArg(OPT_debug_info_kind_EQ);

  if (const Arg *A = Args.getLastArg(OPT_compress_debug_sections,
                                     OPT_compress_debug_sections_EQ)) {
    if (A->getOption().getID() == OPT_compress_debug_sections) {
      // TODO: be more clever about the compression type auto-detection
      Opts.CompressDebugSections = llvm::DebugCompressionType::GNU;
    } else {
      Opts.CompressDebugSections =
          llvm::StringSwitch<llvm::DebugCompressionType>(A->getValue())
              .Case("none", llvm::DebugCompressionType::None)
              .Case("zlib", llvm::DebugCompressionType::Z)
              .Case("zlib-gnu", llvm::DebugCompressionType::GNU)
              .Default(llvm::DebugCompressionType::None);
    }
  }

  Opts.RelaxELFRelocations = Args.hasArg(OPT_mrelax_relocations);
  Opts.DwarfVersion = getLastArgIntValue(Args, OPT_dwarf_version_EQ, 2, Diags);
  Opts.DwarfDebugFlags = Args.getLastArgValue(OPT_dwarf_debug_flags);
  Opts.DwarfDebugProducer = Args.getLastArgValue(OPT_dwarf_debug_producer);
  Opts.DebugCompilationDir = Args.getLastArgValue(OPT_fdebug_compilation_dir);
  Opts.MainFileName = Args.getLastArgValue(OPT_main_file_name);

  for (const auto &Arg : Args.getAllArgValues(OPT_fdebug_prefix_map_EQ))
    Opts.DebugPrefixMap.insert(StringRef(Arg).split('='));

  // Frontend Options
  if (Args.hasArg(OPT_INPUT)) {
    bool First = true;
    for (const Arg *A : Args.filtered(OPT_INPUT)) {
      if (First) {
        Opts.InputFile = A->getValue();
        First = false;
      } else {
        Diags.Report(diag::err_drv_unknown_argument) << A->getAsString(Args);
        Success = false;
      }
    }
  }
  Opts.LLVMArgs = Args.getAllArgValues(OPT_mllvm);
  Opts.OutputPath = Args.getLastArgValue(OPT_o);
  Opts.SplitDwarfFile = Args.getLastArgValue(OPT_split_dwarf_file);
  if (Arg *A = Args.getLastArg(OPT_filetype)) {
    StringRef Name = A->getValue();
    unsigned OutputType = StringSwitch<unsigned>(Name)
      .Case("asm", FT_Asm)
      .Case("null", FT_Null)
      .Case("obj", FT_Obj)
      .Default(~0U);
    if (OutputType == ~0U) {
      Diags.Report(diag::err_drv_invalid_value) << A->getAsString(Args) << Name;
      Success = false;
    } else
      Opts.OutputType = FileType(OutputType);
  }
  Opts.ShowHelp = Args.hasArg(OPT_help);
  Opts.ShowVersion = Args.hasArg(OPT_version);

  // Transliterate Options
  Opts.OutputAsmVariant =
      getLastArgIntValue(Args, OPT_output_asm_variant, 0, Diags);
  Opts.ShowEncoding = Args.hasArg(OPT_show_encoding);
  Opts.ShowInst = Args.hasArg(OPT_show_inst);

  // Assemble Options
  Opts.RelaxAll = Args.hasArg(OPT_mrelax_all);
  Opts.NoExecStack = Args.hasArg(OPT_mno_exec_stack);
  Opts.FatalWarnings = Args.hasArg(OPT_massembler_fatal_warnings);
  Opts.RelocationModel = Args.getLastArgValue(OPT_mrelocation_model, "pic");
  Opts.TargetABI = Args.getLastArgValue(OPT_target_abi);
  Opts.IncrementalLinkerCompatible =
      Args.hasArg(OPT_mincremental_linker_compatible);
  Opts.SymbolDefs = Args.getAllArgValues(OPT_defsym);

  // EmbedBitcode Option. If -fembed-bitcode is enabled, set the flag.
  // EmbedBitcode behaves the same for all embed options for assembly files.
  if (auto *A = Args.getLastArg(OPT_fembed_bitcode_EQ)) {
    Opts.EmbedBitcode = llvm::StringSwitch<unsigned>(A->getValue())
                            .Case("all", 1)
                            .Case("bitcode", 1)
                            .Case("marker", 1)
                            .Default(0);
  }

  return Success;
}

static std::unique_ptr<raw_fd_ostream>
getOutputStream(StringRef Path, DiagnosticsEngine &Diags, bool Binary) {
  // Make sure that the Out file gets unlinked from the disk if we get a
  // SIGINT.
  if (Path != "-")
    sys::RemoveFileOnSignal(Path);

  std::error_code EC;
  auto Out = llvm::make_unique<raw_fd_ostream>(
      Path, EC, (Binary ? sys::fs::F_None : sys::fs::F_Text));
  if (EC) {
    Diags.Report(diag::err_fe_unable_to_open_output) << Path << EC.message();
    return nullptr;
  }

  return Out;
}

// FIXME: dedupe with lib/CodeGen/CodeGenAction.cpp
/// ConvertBackendLocation - Convert a location in a temporary llvm::SourceMgr
/// buffer to be a valid FullSourceLoc.
static FullSourceLoc ConvertBackendLocation(const llvm::SMDiagnostic &D,
                                            SourceManager &CSM) {
  // Get both the clang and llvm source managers.  The location is relative to
  // a memory buffer that the LLVM Source Manager is handling, we need to add
  // a copy to the Clang source manager.
  const llvm::SourceMgr &LSM = *D.getSourceMgr();

  // Sharing buffers here is safe because we always immediately emit the diag,
  // so that the llvm::SourceMgr buffers will outlive the clang::SourceManager
  // ones.
  // FIXME: reuse file ids for same file
  const MemoryBuffer *LBuf =
      LSM.getMemoryBuffer(LSM.FindBufferContainingLoc(D.getLoc()));
  FileID FID = CSM.createFileID(SourceManager::Unowned, LBuf);

  // Translate the offset into the file.
  unsigned Offset = D.getLoc().getPointer() - LBuf->getBufferStart();
  SourceLocation NewLoc =
      CSM.getLocForStartOfFile(FID).getLocWithOffset(Offset);
  return FullSourceLoc(NewLoc, CSM);
}

static void DiagHandler(const llvm::SMDiagnostic &D, void *diagInfo) {
  DiagnosticsEngine &Diags = *static_cast<DiagnosticsEngine*>(diagInfo);

  // FIXME: dedupe with BackendConsumer::InlineAsmDiagHandler2() in
  // lib/CodeGen/CodeGenAction.cpp

  // There are a couple of different kinds of errors we could get here.  First,
  // we re-format the SMDiagnostic in terms of a clang diagnostic.

  // Strip "error: " off the start of the message string.
  StringRef Message = D.getMessage();
  if (Message.startswith("error: "))
    Message = Message.substr(7);

  // If the SMDiagnostic has an inline asm source location, translate it.
  FullSourceLoc Loc;
  if (D.getLoc() != SMLoc())
    Loc = ConvertBackendLocation(D, Diags.getSourceManager());

  // FIXME: Don't sue fe_inline_asm here, use full asm
  unsigned DiagID;
  switch (D.getKind()) {
  case llvm::SourceMgr::DK_Error:
    DiagID = diag::err_fe_inline_asm;
    break;
  case llvm::SourceMgr::DK_Warning:
    DiagID = diag::warn_fe_inline_asm;
    break;
  case llvm::SourceMgr::DK_Note:
    DiagID = diag::note_fe_inline_asm;
    break;
  case llvm::SourceMgr::DK_Remark:
    llvm_unreachable("remarks unexpected");
  }

  Diags.Report(Loc, DiagID).AddString(Message);

#if 0
    AsmPrinter::SrcMgrDiagInfo *DiagInfo =
        static_cast<AsmPrinter::SrcMgrDiagInfo *>(diagInfo);
    assert(DiagInfo && "Diagnostic context not passed down?");

    // Look up a LocInfo for the buffer this diagnostic is coming from.
    unsigned BufNum = DiagInfo->SrcMgr.FindBufferContainingLoc(Diag.getLoc());
    const MDNode *LocInfo = nullptr;
    if (BufNum > 0 && BufNum <= DiagInfo->LocInfos.size())
      LocInfo = DiagInfo->LocInfos[BufNum - 1];

    // If the inline asm had metadata associated with it, pull out a location
    // cookie corresponding to which line the error occurred on.
    unsigned LocCookie = 0;
    if (LocInfo) {
      unsigned ErrorLine = Diag.getLineNo() - 1;
      if (ErrorLine >= LocInfo->getNumOperands())
        ErrorLine = 0;

      if (LocInfo->getNumOperands() != 0)
        if (const ConstantInt *CI = mdconst::dyn_extract<ConstantInt>(
                LocInfo->getOperand(ErrorLine)))
          LocCookie = CI->getZExtValue();
    }

    DiagInfo->DiagHandler(Diag, DiagInfo->DiagContext, LocCookie);
#endif
}

static bool ExecuteAssembler(AssemblerInvocation &Opts,
                             DiagnosticsEngine &Diags) {
  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(Opts.Triple, Error);
  if (!TheTarget)
    return Diags.Report(diag::err_target_unknown_triple) << Opts.Triple;

  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
      MemoryBuffer::getFileOrSTDIN(Opts.InputFile);

  if (std::error_code EC = Buffer.getError()) {
    Error = EC.message();
    return Diags.Report(diag::err_fe_error_reading) << Opts.InputFile;
  }

  llvm::SourceMgr SrcMgr;
  SrcMgr.setDiagHandler(DiagHandler, static_cast<void*>(&Diags));

  // Tell SrcMgr about this buffer, which is what the parser will pick up.
  // XXX both
  clang::SourceManager &CSM = Diags.getSourceManager();

  // XXX needed?
  CSM.setMainFileID(
      CSM.createFileID(SourceManager::Unowned, Buffer->get(), SrcMgr::C_User));

  unsigned BufferIndex = SrcMgr.AddNewSourceBuffer(std::move(*Buffer), SMLoc());

  // Record the location of the include directories so that the lexer can find
  // it later.
  SrcMgr.setIncludeDirs(Opts.IncludePaths);

  std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(Opts.Triple));
  assert(MRI && "Unable to create target register info!");

  std::unique_ptr<MCAsmInfo> MAI(TheTarget->createMCAsmInfo(*MRI, Opts.Triple));
  assert(MAI && "Unable to create target asm info!");

  // Ensure MCAsmInfo initialization occurs before any use, otherwise sections
  // may be created with a combination of default and explicit settings.
  MAI->setCompressDebugSections(Opts.CompressDebugSections);

  MAI->setRelaxELFRelocations(Opts.RelaxELFRelocations);

  bool IsBinary = Opts.OutputType == AssemblerInvocation::FT_Obj;
  if (Opts.OutputPath.empty())
    Opts.OutputPath = "-";
  std::unique_ptr<raw_fd_ostream> FDOS =
      getOutputStream(Opts.OutputPath, Diags, IsBinary);
  if (!FDOS)
    return true;
  std::unique_ptr<raw_fd_ostream> DwoOS;
  if (!Opts.SplitDwarfFile.empty())
    DwoOS = getOutputStream(Opts.SplitDwarfFile, Diags, IsBinary);

  // FIXME: This is not pretty. MCContext has a ptr to MCObjectFileInfo and
  // MCObjectFileInfo needs a MCContext reference in order to initialize itself.
  std::unique_ptr<MCObjectFileInfo> MOFI(new MCObjectFileInfo());

  MCContext Ctx(MAI.get(), MRI.get(), MOFI.get(), &SrcMgr);

  bool PIC = false;
  if (Opts.RelocationModel == "static") {
    PIC = false;
  } else if (Opts.RelocationModel == "pic") {
    PIC = true;
  } else {
    assert(Opts.RelocationModel == "dynamic-no-pic" &&
           "Invalid PIC model!");
    PIC = false;
  }

  MOFI->InitMCObjectFileInfo(Triple(Opts.Triple), PIC, Ctx);
  if (Opts.SaveTemporaryLabels)
    Ctx.setAllowTemporaryLabels(false);
  if (Opts.GenDwarfForAssembly)
    Ctx.setGenDwarfForAssembly(true);
  if (!Opts.DwarfDebugFlags.empty())
    Ctx.setDwarfDebugFlags(StringRef(Opts.DwarfDebugFlags));
  if (!Opts.DwarfDebugProducer.empty())
    Ctx.setDwarfDebugProducer(StringRef(Opts.DwarfDebugProducer));
  if (!Opts.DebugCompilationDir.empty())
    Ctx.setCompilationDir(Opts.DebugCompilationDir);
  else {
    // If no compilation dir is set, try to use the current directory.
    SmallString<128> CWD;
    if (!sys::fs::current_path(CWD))
      Ctx.setCompilationDir(CWD);
  }
  if (!Opts.DebugPrefixMap.empty())
    for (const auto &KV : Opts.DebugPrefixMap)
      Ctx.addDebugPrefixMapEntry(KV.first, KV.second);
  if (!Opts.MainFileName.empty())
    Ctx.setMainFileName(StringRef(Opts.MainFileName));
  Ctx.setDwarfVersion(Opts.DwarfVersion);
  if (Opts.GenDwarfForAssembly)
    Ctx.setGenDwarfRootFile(Opts.InputFile,
                            SrcMgr.getMemoryBuffer(BufferIndex)->getBuffer());

  // Build up the feature string from the target feature list.
  std::string FS;
  if (!Opts.Features.empty()) {
    FS = Opts.Features[0];
    for (unsigned i = 1, e = Opts.Features.size(); i != e; ++i)
      FS += "," + Opts.Features[i];
  }

  std::unique_ptr<MCStreamer> Str;

  std::unique_ptr<MCInstrInfo> MCII(TheTarget->createMCInstrInfo());
  std::unique_ptr<MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(Opts.Triple, Opts.CPU, FS));

  raw_pwrite_stream *Out = FDOS.get();
  std::unique_ptr<buffer_ostream> BOS;

  MCTargetOptions MCOptions;
  MCOptions.ABIName = Opts.TargetABI;

  // FIXME: There is a bit of code duplication with addPassesToEmitFile.
  if (Opts.OutputType == AssemblerInvocation::FT_Asm) {
    MCInstPrinter *IP = TheTarget->createMCInstPrinter(
        llvm::Triple(Opts.Triple), Opts.OutputAsmVariant, *MAI, *MCII, *MRI);

    std::unique_ptr<MCCodeEmitter> CE;
    if (Opts.ShowEncoding)
      CE.reset(TheTarget->createMCCodeEmitter(*MCII, *MRI, Ctx));
    std::unique_ptr<MCAsmBackend> MAB(
        TheTarget->createMCAsmBackend(*STI, *MRI, MCOptions));

    auto FOut = llvm::make_unique<formatted_raw_ostream>(*Out);
    Str.reset(TheTarget->createAsmStreamer(
        Ctx, std::move(FOut), /*asmverbose*/ true,
        /*useDwarfDirectory*/ true, IP, std::move(CE), std::move(MAB),
        Opts.ShowInst));
  } else if (Opts.OutputType == AssemblerInvocation::FT_Null) {
    Str.reset(createNullStreamer(Ctx));
  } else {
    assert(Opts.OutputType == AssemblerInvocation::FT_Obj &&
           "Invalid file type!");
    if (!FDOS->supportsSeeking()) {
      BOS = make_unique<buffer_ostream>(*FDOS);
      Out = BOS.get();
    }

    std::unique_ptr<MCCodeEmitter> CE(
        TheTarget->createMCCodeEmitter(*MCII, *MRI, Ctx));
    std::unique_ptr<MCAsmBackend> MAB(
        TheTarget->createMCAsmBackend(*STI, *MRI, MCOptions));
    std::unique_ptr<MCObjectWriter> OW =
        DwoOS ? MAB->createDwoObjectWriter(*Out, *DwoOS)
              : MAB->createObjectWriter(*Out);

    Triple T(Opts.Triple);
    Str.reset(TheTarget->createMCObjectStreamer(
        T, Ctx, std::move(MAB), std::move(OW), std::move(CE), *STI,
        Opts.RelaxAll, Opts.IncrementalLinkerCompatible,
        /*DWARFMustBeAtTheEnd*/ true));
    Str.get()->InitSections(Opts.NoExecStack);
  }

  // When -fembed-bitcode is passed to clang_as, a 1-byte marker
  // is emitted in __LLVM,__asm section if the object file is MachO format.
  if (Opts.EmbedBitcode && Ctx.getObjectFileInfo()->getObjectFileType() ==
                               MCObjectFileInfo::IsMachO) {
    MCSection *AsmLabel = Ctx.getMachOSection(
        "__LLVM", "__asm", MachO::S_REGULAR, 4, SectionKind::getReadOnly());
    Str.get()->SwitchSection(AsmLabel);
    Str.get()->EmitZeros(1);
  }

  // Assembly to object compilation should leverage assembly info.
  Str->setUseAssemblerInfoForParsing(true);

  bool Failed = false;

  std::unique_ptr<MCAsmParser> Parser(
      createMCAsmParser(SrcMgr, Ctx, *Str.get(), *MAI));

  // FIXME: init MCTargetOptions from sanitizer flags here.
  std::unique_ptr<MCTargetAsmParser> TAP(
      TheTarget->createMCAsmParser(*STI, *Parser, *MCII, MCOptions));
  if (!TAP)
    Failed = Diags.Report(diag::err_target_unknown_triple) << Opts.Triple;

  // Set values for symbols, if any.
  for (auto &S : Opts.SymbolDefs) {
    auto Pair = StringRef(S).split('=');
    auto Sym = Pair.first;
    auto Val = Pair.second;
    int64_t Value;
    // We have already error checked this in the driver.
    Val.getAsInteger(0, Value);
    Ctx.setSymbolValue(Parser->getStreamer(), Sym, Value);
  }

  if (!Failed) {
    Parser->setTargetParser(*TAP.get());
    Failed = Parser->Run(Opts.NoInitialTextSection);
  }

  // Close Streamer first.
  // It might have a reference to the output stream.
  Str.reset();
  // Close the output stream early.
  BOS.reset();
  FDOS.reset();

  // Delete output file if there were errors.
  if (Failed) {
    if (Opts.OutputPath != "-")
      sys::fs::remove(Opts.OutputPath);
    if (!Opts.SplitDwarfFile.empty() && Opts.SplitDwarfFile != "-")
      sys::fs::remove(Opts.SplitDwarfFile);
  }

  return Failed;
}

static void LLVMErrorHandler(void *UserData, const std::string &Message,
                             bool GenCrashDiag) {
  DiagnosticsEngine &Diags = *static_cast<DiagnosticsEngine*>(UserData);

  Diags.Report(diag::err_fe_error_backend) << Message;

  // We cannot recover from llvm errors.
  exit(1);
}

int cc1as_main(ArrayRef<const char *> Argv, const char *Argv0, void *MainAddr) {
  // Initialize targets and assembly printers/parsers.
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();

  // Construct our diagnostic client.
  //IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts =
      CreateAndPopulateDiagOpts(Argv);

  TextDiagnosticPrinter *DiagClient =
      new TextDiagnosticPrinter(errs(), &*DiagOpts);
  LangOptions LO;

  DiagClient->BeginSourceFile(LO);
  DiagClient->setPrefix("clang -cc1as");
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());
  DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagClient);

  // FIXME: last arg value correct?
  ProcessWarningOptions(Diags, *DiagOpts, /*ReportDiags=*/false);

  FileManager FM{FileSystemOptions()};
  SourceManager CSM(Diags, FM);
  Diags.setSourceManager(&CSM);

  // Set an error handler, so that any LLVM backend diagnostics go through our
  // error handler.
  ScopedFatalErrorHandler FatalErrorHandler
    (LLVMErrorHandler, static_cast<void*>(&Diags));

  // Parse the arguments.
  AssemblerInvocation Asm;
  if (!AssemblerInvocation::CreateFromArgs(Asm, Argv, Diags, LO))
    return 1;

  if (Asm.ShowHelp) {
    std::unique_ptr<OptTable> Opts(driver::createDriverOptTable());
    Opts->PrintHelp(llvm::outs(), "clang -cc1as [options] file...",
                    "Clang Integrated Assembler",
                    /*Include=*/driver::options::CC1AsOption, /*Exclude=*/0,
                    /*ShowAllAliases=*/false);
    return 0;
  }

  // Honor -version.
  //
  // FIXME: Use a better -version message?
  if (Asm.ShowVersion) {
    llvm::cl::PrintVersionMessage();
    return 0;
  }

  // Honor -mllvm.
  //
  // FIXME: Remove this, one day.
  if (!Asm.LLVMArgs.empty()) {
    unsigned NumArgs = Asm.LLVMArgs.size();
    auto Args = llvm::make_unique<const char*[]>(NumArgs + 2);
    Args[0] = "clang (LLVM option parsing)";
    for (unsigned i = 0; i != NumArgs; ++i)
      Args[i + 1] = Asm.LLVMArgs[i].c_str();
    Args[NumArgs + 1] = nullptr;
    llvm::cl::ParseCommandLineOptions(NumArgs + 1, Args.get());
  }

  // Execute the invocation, unless there were parsing errors.
  bool Failed = Diags.hasErrorOccurred() || ExecuteAssembler(Asm, Diags);

  // If any timers were active but haven't been destroyed yet, print their
  // results now.
  TimerGroup::printAll(errs());

  return !!Failed;
}
