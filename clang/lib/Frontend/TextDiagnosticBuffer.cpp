//===- TextDiagnosticBuffer.cpp - Buffer Text Diagnostics -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is a concrete diagnostic client, which buffers the diagnostic messages.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"

using namespace clang;

/// HandleDiagnostic - Store the errors, warnings, and notes that are
/// reported.
void TextDiagnosticBuffer::HandleDiagnostic(DiagnosticsEngine::Level Level,
                                            const Diagnostic &Info) {
  // Default implementation (Warnings/errors count).
  DiagnosticConsumer::HandleDiagnostic(Level, Info);

  int PresumedLineNumber = -1;
  if (Info.hasSourceManager()) {
    SourceManager &SM = Info.getSourceManager();
    PresumedLineNumber = SM.getPresumedLineNumber(Info.getLocation());
  }

  SmallString<100> Buf;
  // XXX also need to store file name etc, in case the buffer is gone later on
  Info.FormatDiagnostic(Buf);
  switch (Level) {
  default: llvm_unreachable(
                         "Diagnostic not handled during diagnostic buffering!");
  case DiagnosticsEngine::Note:
    All.emplace_back(Level, Notes.size());
    Notes.emplace_back(Info.getLocation(), PresumedLineNumber, Buf.str());
    break;
  case DiagnosticsEngine::Warning:
    All.emplace_back(Level, Warnings.size());
    Warnings.emplace_back(Info.getLocation(), PresumedLineNumber, Buf.str());
    break;
  case DiagnosticsEngine::Remark:
    All.emplace_back(Level, Remarks.size());
    Remarks.emplace_back(Info.getLocation(), PresumedLineNumber, Buf.str());
    break;
  case DiagnosticsEngine::Error:
  case DiagnosticsEngine::Fatal:
    All.emplace_back(Level, Errors.size());
    Errors.emplace_back(Info.getLocation(), PresumedLineNumber, Buf.str());
    break;
  }
}

void TextDiagnosticBuffer::FlushDiagnostics(DiagnosticsEngine &Diags) const {
  for (const auto &I : All) {
    auto Diag = Diags.Report(Diags.getCustomDiagID(I.first, "%0"));
    switch (I.first) {
    default: llvm_unreachable(
                           "Diagnostic not handled during diagnostic flushing!");
    case DiagnosticsEngine::Note:
      Diag << Notes[I.second].DiagText;
      break;
    case DiagnosticsEngine::Warning:
      Diag << Warnings[I.second].DiagText;
      break;
    case DiagnosticsEngine::Remark:
      Diag << Remarks[I.second].DiagText;
      break;
    case DiagnosticsEngine::Error:
    case DiagnosticsEngine::Fatal:
      Diag << Errors[I.second].DiagText;
      break;
    }
  }
}
