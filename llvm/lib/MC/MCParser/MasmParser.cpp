//===- AsmParser.cpp - Parser for Assembly Files --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This class implements a parser for masm-style assembly.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/Support/SourceMgr.h"

using namespace llvm;

// Note that this is a full MCAsmParser, not an MCAsmParserExtension!
// It's a peer of AsmParser, not of COFFAsmParser, WasmAsmParser, etc.
class MasmParser : public MCAsmParser {
private:
  AsmLexer Lexer;  // FIXME: Probably want custom MCLexer subclass
  MCContext &Ctx;
  MCStreamer &Out;
  const MCAsmInfo &MAI;
  SourceMgr &SrcMgr;
  //SourceMgr::DiagHandlerTy SavedDiagHandler;
  //void *SavedDiagContext;
  //std::unique_ptr<MCAsmParserExtension> PlatformParser;

  /// This is the current buffer index we're lexing from as managed by the
  /// SourceMgr object.
  unsigned CurBuffer;

  //AsmCond TheCondState;
  //std::vector<AsmCond> TheCondStack;

  /// maps directive names to handler methods in parser
  /// extensions. Extensions register themselves in this map by calling
  /// addDirectiveHandler.
  //StringMap<ExtensionDirectiveHandler> ExtensionDirectiveMap;

  /// Stack of active macro instantiations.
  //std::vector<MacroInstantiation*> ActiveMacros;

  /// List of bodies of anonymous macros.
  //std::deque<MCAsmMacro> MacroLikeBodies;

  /// Boolean tracking whether macro substitution is enabled.
  //unsigned MacrosEnabledFlag : 1;

  /// Keeps track of how many .macro's have been instantiated.
  //unsigned NumOfMacroInstantiations;

  /// The values from the last parsed cpp hash file line comment if any.
  //struct CppHashInfoTy {
  //  StringRef Filename;
  //  int64_t LineNumber;
  //  SMLoc Loc;
  //  unsigned Buf;
  //  CppHashInfoTy() : Filename(), LineNumber(0), Loc(), Buf(0) {}
  //};
  //CppHashInfoTy CppHashInfo;

  /// List of forward directional labels for diagnosis at the end.
  //SmallVector<std::tuple<SMLoc, CppHashInfoTy, MCSymbol *>, 4> DirLabels;

  /// AssemblerDialect. ~OU means unset value and use value provided by MAI.
  //unsigned AssemblerDialect = ~0U;

  /// is Darwin compatibility enabled?
  //bool IsDarwin = false;

  /// Are we parsing ms-style inline assembly?
  //bool ParsingInlineAsm = false;

  /// Did we already inform the user about inconsistent MD5 usage?
  //bool ReportedInconsistentMD5 = false;

  // Is alt macro mode enabled.
  //bool AltMacroMode = false;

public:
  MasmParser(SourceMgr &SM, MCContext &Ctx, MCStreamer &Out,
                const MCAsmInfo &MAI, unsigned CB);
  MasmParser(const MasmParser &) = delete;
  MasmParser &operator=(const MasmParser &) = delete;
  ~MasmParser() override;

  /// @name MCAsmParser Interface
  /// {

  bool Run(bool NoInitialTextSection, bool NoFinalize = false) override;

  void addDirectiveHandler(StringRef Directive,
                           ExtensionDirectiveHandler Handler) override {
    assert(false && "not supported for MasmParser");
  }

  void addAliasForDirective(StringRef Directive, StringRef Alias) override {
    assert(false && "not supported for MasmParser");
  }

  SourceMgr &getSourceManager() override { return SrcMgr; }
  MCAsmLexer &getLexer() override { return Lexer; }
  MCContext &getContext() override { return Ctx; }
  MCStreamer &getStreamer() override { return Out; }

  //CodeViewContext &getCVContext() { return Ctx.getCVContext(); }

  unsigned getAssemblerDialect() override {
    assert(false && "not supported for MasmParser");
    return 0;
  }
  void setAssemblerDialect(unsigned i) override {
    assert(false && "not supported for MasmParser");
  }

  void Note(SMLoc L, const Twine &Msg, SMRange Range = None) override;
  bool Warning(SMLoc L, const Twine &Msg, SMRange Range = None) override;
  bool printError(SMLoc L, const Twine &Msg, SMRange Range = None) override;

  const AsmToken &Lex() override;

  void setParsingInlineAsm(bool V) override {
    assert(!false);
    Lexer.setLexMasmIntegers(true);
  }
  bool isParsingInlineAsm() override { return false; }

  bool parseMSInlineAsm(void *AsmLoc, std::string &AsmString,
                        unsigned &NumOutputs, unsigned &NumInputs,
                        SmallVectorImpl<std::pair<void *,bool>> &OpDecls,
                        SmallVectorImpl<std::string> &Constraints,
                        SmallVectorImpl<std::string> &Clobbers,
                        const MCInstrInfo *MII, const MCInstPrinter *IP,
                        MCAsmParserSemaCallback &SI) override;

  //bool parseExpression(const MCExpr *&Res);
  bool parseExpression(const MCExpr *&Res, SMLoc &EndLoc) override;
  bool parsePrimaryExpr(const MCExpr *&Res, SMLoc &EndLoc) override;
  bool parseParenExpression(const MCExpr *&Res, SMLoc &EndLoc) override;
  bool parseParenExprOfDepth(unsigned ParenDepth, const MCExpr *&Res,
                             SMLoc &EndLoc) override;
  bool parseAbsoluteExpression(int64_t &Res) override;

  /// Parse a floating point expression using the float \p Semantics
  /// and set \p Res to the value.
  //bool parseRealValue(const fltSemantics &Semantics, APInt &Res);

  /// Parse an identifier or string (as a quoted identifier)
  /// and set \p Res to the identifier contents.
  bool parseIdentifier(StringRef &Res) override;
  void eatToEndOfStatement() override;

  bool checkForValidSection() override;

  /// Parse up to the end of statement and a return the contents from the
  /// current token until the end of the statement; the current token on exit
  /// will be either the EndOfStatement or EOF.
  StringRef parseStringToEndOfStatement() override;

  /// }

private:
  //bool parseStatement(ParseStatementInfo &Info,
                      //MCAsmParserSemaCallback *SI);
  //bool parseCurlyBlockScope(SmallVectorImpl<AsmRewrite>& AsmStrRewrites);
  //bool parseCppHashLineFilenameComment(SMLoc L);

  //void checkForBadMacro(SMLoc DirectiveLoc, StringRef Name, StringRef Body,
                        //ArrayRef<MCAsmMacroParameter> Parameters);
  // bool expandMacro(raw_svector_ostream &OS, StringRef Body,
  // ArrayRef<MCAsmMacroParameter> Parameters,
  // ArrayRef<MCAsmMacroArgument> A, bool EnableAtPseudoVariable,
  // SMLoc L);

  /// Are macros enabled in the parser?
  //bool areMacrosEnabled() {return MacrosEnabledFlag;}

  /// Control a flag in the parser that enables or disables macros.
  //void setMacrosEnabled(bool Flag) {MacrosEnabledFlag = Flag;}

  /// Are we inside a macro instantiation?
  //bool isInsideMacroInstantiation() {return !ActiveMacros.empty();}

  /// Handle entry to macro instantiation.
  ///
  /// \param M The macro.
  /// \param NameLoc Instantiation location.
  //bool handleMacroEntry(const MCAsmMacro *M, SMLoc NameLoc);

  /// Handle exit from macro instantiation.
  //void handleMacroExit();

  /// Extract AsmTokens for a macro argument.
  //bool parseMacroArgument(MCAsmMacroArgument &MA, bool Vararg);

  /// Parse all macro arguments for a given macro.
  //bool parseMacroArguments(const MCAsmMacro *M, MCAsmMacroArguments &A);

  //void printMacroInstantiations();
  //void printMessage(SMLoc Loc, SourceMgr::DiagKind Kind, const Twine &Msg,
                    //SMRange Range = None) const {
    //ArrayRef<SMRange> Ranges(Range);
    //SrcMgr.PrintMessage(Loc, Kind, Msg, Ranges);
  //}
  //static void DiagHandler(const SMDiagnostic &Diag, void *Context);

  /// Should we emit DWARF describing this assembler source?  (Returns false if
  /// the source has .file directives, which means we don't want to generate
  /// info describing the assembler source itself.)
  //bool enabledGenDwarfForAssembly();

  /// Enter the specified file. This returns true on failure.
  //bool enterIncludeFile(const std::string &Filename);

  /// Process the specified file for the .incbin directive.
  /// This returns true on failure.
  //bool processIncbinFile(const std::string &Filename, int64_t Skip = 0,
                         //const MCExpr *Count = nullptr, SMLoc Loc = SMLoc());

  /// Reset the current lexer position to that given by \p Loc. The
  /// current token is not set; clients should ensure Lex() is called
  /// subsequently.
  ///
  /// \param InBuffer If not 0, should be the known buffer id that contains the
  /// location.
  //void jumpToLoc(SMLoc Loc, unsigned InBuffer = 0);

  /// Parse until the end of a statement or a comma is encountered,
  /// return the contents from the current token up to the end or comma.
  //StringRef parseStringToComma();

  //bool parseAssignment(StringRef Name, bool allow_redef,
                       //bool NoDeadStrip = false);

  //unsigned getBinOpPrecedence(AsmToken::TokenKind K,
                              //MCBinaryExpr::Opcode &Kind);

  //bool parseBinOpRHS(unsigned Precedence, const MCExpr *&Res, SMLoc &EndLoc);
  //bool parseParenExpr(const MCExpr *&Res, SMLoc &EndLoc);
  //bool parseBracketExpr(const MCExpr *&Res, SMLoc &EndLoc);

  //bool parseRegisterOrRegisterNumber(int64_t &Register, SMLoc DirectiveLoc);

  //bool parseCVFunctionId(int64_t &FunctionId, StringRef DirectiveName);
  //bool parseCVFileId(int64_t &FileId, StringRef DirectiveName);

  // Generic (target and platform independent) directive parsing.
  //enum DirectiveKind {
  //  DK_NO_DIRECTIVE, // Placeholder
  //  DK_SET,
  //  DK_EQU,
  //  DK_EQUIV,
  //  DK_ASCII,
  //  DK_ASCIZ,
  //  DK_STRING,
  //  DK_BYTE,
  //  DK_SHORT,
  //  DK_RELOC,
  //  DK_VALUE,
  //  DK_2BYTE,
  //  DK_LONG,
  //  DK_INT,
  //  DK_4BYTE,
  //  DK_QUAD,
  //  DK_8BYTE,
  //  DK_OCTA,
  //  DK_DC,
  //  DK_DC_A,
  //  DK_DC_B,
  //  DK_DC_D,
  //  DK_DC_L,
  //  DK_DC_S,
  //  DK_DC_W,
  //  DK_DC_X,
  //  DK_DCB,
  //  DK_DCB_B,
  //  DK_DCB_D,
  //  DK_DCB_L,
  //  DK_DCB_S,
  //  DK_DCB_W,
  //  DK_DCB_X,
  //  DK_DS,
  //  DK_DS_B,
  //  DK_DS_D,
  //  DK_DS_L,
  //  DK_DS_P,
  //  DK_DS_S,
  //  DK_DS_W,
  //  DK_DS_X,
  //  DK_SINGLE,
  //  DK_FLOAT,
  //  DK_DOUBLE,
  //  DK_ALIGN,
  //  DK_ALIGN32,
  //  DK_BALIGN,
  //  DK_BALIGNW,
  //  DK_BALIGNL,
  //  DK_P2ALIGN,
  //  DK_P2ALIGNW,
  //  DK_P2ALIGNL,
  //  DK_ORG,
  //  DK_FILL,
  //  DK_ENDR,
  //  DK_BUNDLE_ALIGN_MODE,
  //  DK_BUNDLE_LOCK,
  //  DK_BUNDLE_UNLOCK,
  //  DK_ZERO,
  //  DK_EXTERN,
  //  DK_GLOBL,
  //  DK_GLOBAL,
  //  DK_LAZY_REFERENCE,
  //  DK_NO_DEAD_STRIP,
  //  DK_SYMBOL_RESOLVER,
  //  DK_PRIVATE_EXTERN,
  //  DK_REFERENCE,
  //  DK_WEAK_DEFINITION,
  //  DK_WEAK_REFERENCE,
  //  DK_WEAK_DEF_CAN_BE_HIDDEN,
  //  DK_COLD,
  //  DK_COMM,
  //  DK_COMMON,
  //  DK_LCOMM,
  //  DK_ABORT,
  //  DK_INCLUDE,
  //  DK_INCBIN,
  //  DK_CODE16,
  //  DK_CODE16GCC,
  //  DK_REPT,
  //  DK_IRP,
  //  DK_IRPC,
  //  DK_IF,
  //  DK_IFEQ,
  //  DK_IFGE,
  //  DK_IFGT,
  //  DK_IFLE,
  //  DK_IFLT,
  //  DK_IFNE,
  //  DK_IFB,
  //  DK_IFNB,
  //  DK_IFC,
  //  DK_IFEQS,
  //  DK_IFNC,
  //  DK_IFNES,
  //  DK_IFDEF,
  //  DK_IFNDEF,
  //  DK_IFNOTDEF,
  //  DK_ELSEIF,
  //  DK_ELSE,
  //  DK_ENDIF,
  //  DK_SPACE,
  //  DK_SKIP,
  //  DK_FILE,
  //  DK_LINE,
  //  DK_LOC,
  //  DK_STABS,
  //  DK_CV_FILE,
  //  DK_CV_FUNC_ID,
  //  DK_CV_INLINE_SITE_ID,
  //  DK_CV_LOC,
  //  DK_CV_LINETABLE,
  //  DK_CV_INLINE_LINETABLE,
  //  DK_CV_DEF_RANGE,
  //  DK_CV_STRINGTABLE,
  //  DK_CV_STRING,
  //  DK_CV_FILECHECKSUMS,
  //  DK_CV_FILECHECKSUM_OFFSET,
  //  DK_CV_FPO_DATA,
  //  DK_CFI_SECTIONS,
  //  DK_CFI_STARTPROC,
  //  DK_CFI_ENDPROC,
  //  DK_CFI_DEF_CFA,
  //  DK_CFI_DEF_CFA_OFFSET,
  //  DK_CFI_ADJUST_CFA_OFFSET,
  //  DK_CFI_DEF_CFA_REGISTER,
  //  DK_CFI_OFFSET,
  //  DK_CFI_REL_OFFSET,
  //  DK_CFI_PERSONALITY,
  //  DK_CFI_LSDA,
  //  DK_CFI_REMEMBER_STATE,
  //  DK_CFI_RESTORE_STATE,
  //  DK_CFI_SAME_VALUE,
  //  DK_CFI_RESTORE,
  //  DK_CFI_ESCAPE,
  //  DK_CFI_RETURN_COLUMN,
  //  DK_CFI_SIGNAL_FRAME,
  //  DK_CFI_UNDEFINED,
  //  DK_CFI_REGISTER,
  //  DK_CFI_WINDOW_SAVE,
  //  DK_CFI_B_KEY_FRAME,
  //  DK_MACROS_ON,
  //  DK_MACROS_OFF,
  //  DK_ALTMACRO,
  //  DK_NOALTMACRO,
  //  DK_MACRO,
  //  DK_EXITM,
  //  DK_ENDM,
  //  DK_ENDMACRO,
  //  DK_PURGEM,
  //  DK_SLEB128,
  //  DK_ULEB128,
  //  DK_ERR,
  //  DK_ERROR,
  //  DK_WARNING,
  //  DK_PRINT,
  //  DK_ADDRSIG,
  //  DK_ADDRSIG_SYM,
  //  DK_END
  //};

  /// Maps directive name --> DirectiveKind enum, for
  /// directives parsed by this class.
  //StringMap<DirectiveKind> DirectiveKindMap;

  // ".ascii", ".asciz", ".string"
  //bool parseDirectiveAscii(StringRef IDVal, bool ZeroTerminated);
  //bool parseDirectiveReloc(SMLoc DirectiveLoc); // ".reloc"
  //bool parseDirectiveValue(StringRef IDVal,
  //                         unsigned Size);       // ".byte", ".long", ...
  //bool parseDirectiveOctaValue(StringRef IDVal); // ".octa", ...
  //bool parseDirectiveRealValue(StringRef IDVal,
                               //const fltSemantics &); // ".single", ...
  //bool parseDirectiveFill(); // ".fill"
  //bool parseDirectiveZero(); // ".zero"
  // ".set", ".equ", ".equiv"
  //bool parseDirectiveSet(StringRef IDVal, bool allow_redef);
  //bool parseDirectiveOrg(); // ".org"
  // ".align{,32}", ".p2align{,w,l}"
  //bool parseDirectiveAlign(bool IsPow2, unsigned ValueSize);

  // ".file", ".line", ".loc", ".stabs"
  //bool parseDirectiveFile(SMLoc DirectiveLoc);
  //bool parseDirectiveLine();
  //bool parseDirectiveLoc();
  //bool parseDirectiveStabs();

  // ".cv_file", ".cv_func_id", ".cv_inline_site_id", ".cv_loc", ".cv_linetable",
  // ".cv_inline_linetable", ".cv_def_range", ".cv_string"
  //bool parseDirectiveCVFile();
  //bool parseDirectiveCVFuncId();
  //bool parseDirectiveCVInlineSiteId();
  //bool parseDirectiveCVLoc();
  //bool parseDirectiveCVLinetable();
  //bool parseDirectiveCVInlineLinetable();
  //bool parseDirectiveCVDefRange();
  //bool parseDirectiveCVString();
  //bool parseDirectiveCVStringTable();
  //bool parseDirectiveCVFileChecksums();
  //bool parseDirectiveCVFileChecksumOffset();
  //bool parseDirectiveCVFPOData();

  // .cfi directives
  //bool parseDirectiveCFIRegister(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIWindowSave();
  //bool parseDirectiveCFISections();
  //bool parseDirectiveCFIStartProc();
  //bool parseDirectiveCFIEndProc();
  //bool parseDirectiveCFIDefCfaOffset();
  //bool parseDirectiveCFIDefCfa(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIAdjustCfaOffset();
  //bool parseDirectiveCFIDefCfaRegister(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIOffset(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIRelOffset(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIPersonalityOrLsda(bool IsPersonality);
  //bool parseDirectiveCFIRememberState();
  //bool parseDirectiveCFIRestoreState();
  //bool parseDirectiveCFISameValue(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIRestore(SMLoc DirectiveLoc);
  //bool parseDirectiveCFIEscape();
  //bool parseDirectiveCFIReturnColumn(SMLoc DirectiveLoc);
  //bool parseDirectiveCFISignalFrame();
  //bool parseDirectiveCFIUndefined(SMLoc DirectiveLoc);

  // macro directives
  //bool parseDirectivePurgeMacro(SMLoc DirectiveLoc);
  //bool parseDirectiveExitMacro(StringRef Directive);
  //bool parseDirectiveEndMacro(StringRef Directive);
  //bool parseDirectiveMacro(SMLoc DirectiveLoc);
  //bool parseDirectiveMacrosOnOff(StringRef Directive);
  // alternate macro mode directives
  //bool parseDirectiveAltmacro(StringRef Directive);
  // ".bundle_align_mode"
  //bool parseDirectiveBundleAlignMode();
  // ".bundle_lock"
  //bool parseDirectiveBundleLock();
  // ".bundle_unlock"
  //bool parseDirectiveBundleUnlock();

  // ".space", ".skip"
  //bool parseDirectiveSpace(StringRef IDVal);

  // ".dcb"
  //bool parseDirectiveDCB(StringRef IDVal, unsigned Size);
  //bool parseDirectiveRealDCB(StringRef IDVal, const fltSemantics &);
  // ".ds"
  //bool parseDirectiveDS(StringRef IDVal, unsigned Size);

  // .sleb128 (Signed=true) and .uleb128 (Signed=false)
  //bool parseDirectiveLEB128(bool Signed);

  /// Parse a directive like ".globl" which
  /// accepts a single symbol (which should be a label or an external).
  //bool parseDirectiveSymbolAttribute(MCSymbolAttr Attr);

  //bool parseDirectiveComm(bool IsLocal); // ".comm" and ".lcomm"

  //bool parseDirectiveAbort(); // ".abort"
  //bool parseDirectiveInclude(); // ".include"
  //bool parseDirectiveIncbin(); // ".incbin"

  // ".if", ".ifeq", ".ifge", ".ifgt" , ".ifle", ".iflt" or ".ifne"
  //bool parseDirectiveIf(SMLoc DirectiveLoc, DirectiveKind DirKind);
  // ".ifb" or ".ifnb", depending on ExpectBlank.
  //bool parseDirectiveIfb(SMLoc DirectiveLoc, bool ExpectBlank);
  // ".ifc" or ".ifnc", depending on ExpectEqual.
  //bool parseDirectiveIfc(SMLoc DirectiveLoc, bool ExpectEqual);
  // ".ifeqs" or ".ifnes", depending on ExpectEqual.
  //bool parseDirectiveIfeqs(SMLoc DirectiveLoc, bool ExpectEqual);
  // ".ifdef" or ".ifndef", depending on expect_defined
  //bool parseDirectiveIfdef(SMLoc DirectiveLoc, bool expect_defined);
  //bool parseDirectiveElseIf(SMLoc DirectiveLoc); // ".elseif"
  //bool parseDirectiveElse(SMLoc DirectiveLoc); // ".else"
  //bool parseDirectiveEndIf(SMLoc DirectiveLoc); // .endif
  bool parseEscapedString(std::string &Data) override;

  //const MCExpr *applyModifierToExpr(const MCExpr *E,
                                    //MCSymbolRefExpr::VariantKind Variant);

  // Macro-like directives
  //MCAsmMacro *parseMacroLikeBody(SMLoc DirectiveLoc);
  //void instantiateMacroLikeBody(MCAsmMacro *M, SMLoc DirectiveLoc,
                                //raw_svector_ostream &OS);
  //bool parseDirectiveRept(SMLoc DirectiveLoc, StringRef Directive);
  //bool parseDirectiveIrp(SMLoc DirectiveLoc);  // ".irp"
  //bool parseDirectiveIrpc(SMLoc DirectiveLoc); // ".irpc"
  //bool parseDirectiveEndr(SMLoc DirectiveLoc); // ".endr"

  // "_emit" or "__emit"
  //bool parseDirectiveMSEmit(SMLoc DirectiveLoc, ParseStatementInfo &Info,
                            //size_t Len);

  // "align"
  //bool parseDirectiveMSAlign(SMLoc DirectiveLoc, ParseStatementInfo &Info);

  // "end"
  //bool parseDirectiveEnd(SMLoc DirectiveLoc);

  // ".err" or ".error"
  //bool parseDirectiveError(SMLoc DirectiveLoc, bool WithMessage);

  // ".warning"
  //bool parseDirectiveWarning(SMLoc DirectiveLoc);

  // .print <double-quotes-string>
  //bool parseDirectivePrint(SMLoc DirectiveLoc);

  // Directives to support address-significance tables.
  //bool parseDirectiveAddrsig();
  //bool parseDirectiveAddrsigSym();

  //void initializeDirectiveKindMap();
};

MasmParser::MasmParser(SourceMgr &SM, MCContext &Ctx, MCStreamer &Out,
                       const MCAsmInfo &MAI, unsigned CB = 0)
    : Lexer(MAI), Ctx(Ctx), Out(Out), MAI(MAI), SrcMgr(SM),
      CurBuffer(CB ? CB : SM.getMainFileID())  {
#if 0
  HadError = false;
  // Save the old handler.
  SavedDiagHandler = SrcMgr.getDiagHandler();
  SavedDiagContext = SrcMgr.getDiagContext();
  // Set our own handler which calls the saved handler.
  SrcMgr.setDiagHandler(DiagHandler, this);
  Lexer.setBuffer(SrcMgr.getMemoryBuffer(CurBuffer)->getBuffer());

  // Initialize the platform / file format parser.
  switch (Ctx.getObjectFileInfo()->getObjectFileType()) {
  case MCObjectFileInfo::IsCOFF:
    PlatformParser.reset(createCOFFAsmParser());
    break;
  case MCObjectFileInfo::IsMachO:
    PlatformParser.reset(createDarwinAsmParser());
    IsDarwin = true;
    break;
  case MCObjectFileInfo::IsELF:
    PlatformParser.reset(createELFAsmParser());
    break;
  case MCObjectFileInfo::IsWasm:
    PlatformParser.reset(createWasmAsmParser());
    break;
  }

  PlatformParser->Initialize(*this);
  initializeDirectiveKindMap();

  NumOfMacroInstantiations = 0;
#endif
}

/// Create an MCAsmParser instance.
MCAsmParser *llvm::createMCMasmParser(SourceMgr &SM, MCContext &C,
                                         MCStreamer &Out, const MCAsmInfo &MAI,
                                         unsigned CB) {
  return new MasmParser(SM, C, Out, MAI, CB);
}
