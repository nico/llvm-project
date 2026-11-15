//===- LineTable.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A line table cannot be relocated in place. The compiler emits one sequence
// of rows for all the code of an object file's text section, but the linker
// places each function on its own and drops some. So the line number program
// is run, the rows of functions that are not in the binary are dropped, and
// the others are encoded again as one sequence per function at its new
// address. The header, with the directory and file tables, is kept as it is,
// so that file numbers elsewhere in the debug info stay valid.
//
//===----------------------------------------------------------------------===//

#include "LineTable.h"
#include "Dwarf.h"
#include "ObjectFile.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;
using namespace llvm::dsymutil2;
using namespace llvm::support::endian;

namespace {
/// The registers of the line number state machine that make up a row.
struct Row {
  uint64_t Address = 0;
  uint64_t File = 1;
  uint64_t Line = 1;
  uint64_t Column = 0;
  uint64_t Isa = 0;
  uint64_t Discriminator = 0;
  bool IsStmt = false;
  bool BasicBlock = false;
  bool PrologueEnd = false;
  bool EpilogueBegin = false;
};

/// The parameters of a line number program, from the table's header.
struct Parameters {
  uint16_t Version;
  uint8_t MinInstLength;
  bool DefaultIsStmt;
  int8_t LineBase;
  uint8_t LineRange;
  uint8_t OpcodeBase;
  const uint8_t *StandardOpcodeLengths;
};

/// Encodes rows as line number programs, a sequence per function.
class Encoder {
public:
  Encoder(const ObjectFile &Object, const Parameters &Params,
          SmallVectorImpl<char> &Out)
      : Object(Object), Params(Params), Out(Out) {}

  /// Takes a row of the input program, with its address in the object file.
  void addRow(const Row &Input);
  /// Takes the end of a sequence of the input program at \p Address.
  void endInputSequence(uint64_t Address);

private:
  void endSequence(uint64_t LinkedAddress);
  void emitByte(uint8_t Byte) { Out.push_back(Byte); }
  void emitULEB(uint64_t Value) {
    uint8_t Buffer[10];
    unsigned Size = encodeULEB128(Value, Buffer);
    Out.append(Buffer, Buffer + Size);
  }
  void emitSLEB(int64_t Value) {
    uint8_t Buffer[10];
    unsigned Size = encodeSLEB128(Value, Buffer);
    Out.append(Buffer, Buffer + Size);
  }

  const ObjectFile &Object;
  const Parameters &Params;
  SmallVectorImpl<char> &Out;
  /// The function whose sequence is being written, if any, and the state of
  /// the output program in it.
  const AddressRange *Function = nullptr;
  Row State;
};
} // namespace

void Encoder::endSequence(uint64_t LinkedAddress) {
  // A sequence must end after its last row.
  LinkedAddress =
      std::max(LinkedAddress, State.Address + Params.MinInstLength);
  emitByte(dwarf::DW_LNS_advance_pc);
  emitULEB((LinkedAddress - State.Address) / Params.MinInstLength);
  emitByte(0);
  emitULEB(1);
  emitByte(dwarf::DW_LNE_end_sequence);
  Function = nullptr;
}

void Encoder::endInputSequence(uint64_t Address) {
  if (!Function)
    return;
  // The input sequence usually ends where its last function ends.
  uint64_t End = std::min(Address, Function->End);
  if (End <= Function->Begin)
    End = Function->End;
  endSequence(End - Function->Begin + Function->Linked);
}

void Encoder::addRow(const Row &Input) {
  if (!Function || Input.Address < Function->Begin ||
      Input.Address >= Function->End) {
    // The row is in another function than the one before.
    if (Function)
      endSequence(Function->End - Function->Begin + Function->Linked);
    const AddressRange *Range = Object.findContaining(Input.Address);
    if (!Range)
      return;
    Function = Range;
    State = Row();
    State.IsStmt = Params.DefaultIsStmt;
    State.Address = Input.Address - Range->Begin + Range->Linked;
    emitByte(0);
    emitULEB(1 + sizeof(uint64_t));
    emitByte(dwarf::DW_LNE_set_address);
    char Address[sizeof(uint64_t)];
    write64le(Address, State.Address);
    Out.append(Address, Address + sizeof(Address));
  }
  uint64_t Address = Input.Address - Function->Begin + Function->Linked;

  if (Input.File != State.File) {
    emitByte(dwarf::DW_LNS_set_file);
    emitULEB(Input.File);
  }
  if (Input.Column != State.Column) {
    emitByte(dwarf::DW_LNS_set_column);
    emitULEB(Input.Column);
  }
  if (Input.IsStmt != State.IsStmt)
    emitByte(dwarf::DW_LNS_negate_stmt);
  if (Input.Isa != State.Isa && Params.OpcodeBase > dwarf::DW_LNS_set_isa) {
    emitByte(dwarf::DW_LNS_set_isa);
    emitULEB(Input.Isa);
  }
  if (Input.BasicBlock)
    emitByte(dwarf::DW_LNS_set_basic_block);
  if (Input.PrologueEnd && Params.OpcodeBase > dwarf::DW_LNS_set_prologue_end)
    emitByte(dwarf::DW_LNS_set_prologue_end);
  if (Input.EpilogueBegin &&
      Params.OpcodeBase > dwarf::DW_LNS_set_epilogue_begin)
    emitByte(dwarf::DW_LNS_set_epilogue_begin);
  if (Input.Discriminator && Params.Version >= 4) {
    emitByte(0);
    emitULEB(1 + getULEB128Size(Input.Discriminator));
    emitByte(dwarf::DW_LNE_set_discriminator);
    emitULEB(Input.Discriminator);
  }

  // Advance the line and the address and emit the row, with one special
  // opcode if the two advances are small enough for that.
  int64_t LineAdvance = int64_t(Input.Line) - int64_t(State.Line);
  uint64_t AddressAdvance = (Address - State.Address) / Params.MinInstLength;
  bool LineFits = Params.LineRange != 0 && LineAdvance >= Params.LineBase &&
                  LineAdvance < Params.LineBase + Params.LineRange;
  if (!LineFits) {
    if (LineAdvance) {
      emitByte(dwarf::DW_LNS_advance_line);
      emitSLEB(LineAdvance);
    }
    LineAdvance = 0;
    LineFits = Params.LineRange != 0 && Params.LineBase <= 0 &&
               Params.LineBase + Params.LineRange > 0;
  }
  uint64_t Opcode = 256;
  if (LineFits) {
    Opcode = (LineAdvance - Params.LineBase) + Params.OpcodeBase;
    uint64_t WithAddress = Opcode + Params.LineRange * AddressAdvance;
    if (AddressAdvance <= 255 && WithAddress <= 255) {
      Opcode = WithAddress;
      AddressAdvance = 0;
    }
  }
  if (AddressAdvance) {
    emitByte(dwarf::DW_LNS_advance_pc);
    emitULEB(AddressAdvance);
  }
  emitByte(Opcode <= 255 ? Opcode : dwarf::DW_LNS_copy);

  State = Input;
  State.Address = Address;
  State.BasicBlock = State.PrologueEnd = State.EpilogueBegin = false;
  State.Discriminator = 0;
}

const char *dsymutil2::linkLineTable(const ObjectFile &Object, uint64_t Offset,
                                     SmallVectorImpl<char> &Out) {
  StringRef Section = Object.Line;
  if (Offset >= Section.size() || Section.size() - Offset < 16)
    return "line table offset is outside of __debug_line";
  const uint8_t *Table = Section.bytes_begin() + Offset;
  uint64_t Length = read32le(Table);
  if (Length == dwarf::DW_LENGTH_DWARF64)
    return "DWARF64 line tables are not supported";
  if (Length > Section.size() - Offset - 4)
    return "line table extends past the end of __debug_line";
  const uint8_t *End = Table + 4 + Length;

  Parameters Params;
  Params.Version = read16le(Table + 4);
  if (Params.Version < 2 || Params.Version > 4)
    return "unsupported line table version";
  uint64_t HeaderLength = read32le(Table + 6);
  const uint8_t *P = Table + 10;
  if (HeaderLength > uint64_t(End - P))
    return "line table header extends past the end of the table";
  const uint8_t *Program = P + HeaderLength;
  Params.MinInstLength = *P++;
  if (Params.Version >= 4)
    ++P; // maximum_operations_per_instruction
  Params.DefaultIsStmt = *P++;
  Params.LineBase = *P++;
  Params.LineRange = *P++;
  Params.OpcodeBase = *P++;
  Params.StandardOpcodeLengths = P;
  if (Params.MinInstLength == 0 || Params.OpcodeBase == 0 ||
      P + (Params.OpcodeBase - 1) > Program)
    return "malformed line table header";

  // The header is kept; its length field is filled in at the end.
  size_t Start = Out.size();
  Out.append(Table, Program);

  Encoder Sequences(Object, Params, Out);
  Row State;
  State.IsStmt = Params.DefaultIsStmt;
  auto EmitRow = [&] {
    Sequences.addRow(State);
    State.BasicBlock = State.PrologueEnd = State.EpilogueBegin = false;
    State.Discriminator = 0;
  };
  // Operands are read without checking against End. They are LEB128s that a
  // few bytes at most extend past it, where the file is still mapped.
  for (P = Program; P < End;) {
    uint8_t Opcode = *P++;
    if (Opcode >= Params.OpcodeBase) {
      uint8_t Adjusted = Opcode - Params.OpcodeBase;
      if (Params.LineRange) {
        State.Address +=
            uint64_t(Adjusted / Params.LineRange) * Params.MinInstLength;
        State.Line += Params.LineBase + Adjusted % Params.LineRange;
      }
      EmitRow();
      continue;
    }
    switch (Opcode) {
    case 0: {
      uint64_t Size = readULEB(P);
      if (Size == 0 || Size > uint64_t(End - P)) {
        Out.truncate(Start);
        return "malformed extended opcode in line table";
      }
      switch (*P) {
      case dwarf::DW_LNE_end_sequence:
        Sequences.endInputSequence(State.Address);
        State = Row();
        State.IsStmt = Params.DefaultIsStmt;
        break;
      case dwarf::DW_LNE_set_address:
        if (Size == 1 + 8)
          State.Address = read64le(P + 1);
        else if (Size == 1 + 4)
          State.Address = read32le(P + 1);
        break;
      case dwarf::DW_LNE_set_discriminator: {
        const uint8_t *Operand = P + 1;
        State.Discriminator = readULEB(Operand);
        break;
      }
      default:
        // DW_LNE_define_file would have to be carried over to the output.
        // Compilers do not use it.
        break;
      }
      P += Size;
      break;
    }
    case dwarf::DW_LNS_copy:
      EmitRow();
      break;
    case dwarf::DW_LNS_advance_pc:
      State.Address += readULEB(P) * Params.MinInstLength;
      break;
    case dwarf::DW_LNS_advance_line: {
      unsigned Size;
      State.Line += decodeSLEB128(P, &Size);
      P += Size;
      break;
    }
    case dwarf::DW_LNS_set_file:
      State.File = readULEB(P);
      break;
    case dwarf::DW_LNS_set_column:
      State.Column = readULEB(P);
      break;
    case dwarf::DW_LNS_negate_stmt:
      State.IsStmt = !State.IsStmt;
      break;
    case dwarf::DW_LNS_set_basic_block:
      State.BasicBlock = true;
      break;
    case dwarf::DW_LNS_const_add_pc:
      if (Params.LineRange)
        State.Address += uint64_t((255 - Params.OpcodeBase) /
                                  Params.LineRange) *
                         Params.MinInstLength;
      break;
    case dwarf::DW_LNS_fixed_advance_pc:
      State.Address += read16le(P);
      P += 2;
      break;
    case dwarf::DW_LNS_set_prologue_end:
      State.PrologueEnd = true;
      break;
    case dwarf::DW_LNS_set_epilogue_begin:
      State.EpilogueBegin = true;
      break;
    case dwarf::DW_LNS_set_isa:
      State.Isa = readULEB(P);
      break;
    default:
      // A standard opcode this code does not know: skip its operands.
      for (unsigned I = 0; I != Params.StandardOpcodeLengths[Opcode - 1]; ++I)
        skipLEB(P);
      break;
    }
  }
  // Functions whose rows were not followed by an end of sequence.
  Sequences.endInputSequence(State.Address);

  write32le(Out.data() + Start, Out.size() - Start - 4);
  return nullptr;
}
