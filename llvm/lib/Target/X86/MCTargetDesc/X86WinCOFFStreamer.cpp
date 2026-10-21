//===-- X86WinCOFFStreamer.cpp - X86 Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/X86TargetInfo.h"
#include "X86BaseInfo.h"
#include "X86MCTargetDesc.h"
#include "X86TargetStreamer.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCWin64EH.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace {
class X86WinCOFFStreamer : public MCWinCOFFStreamer {
  Win64EH::UnwindEmitter EHStreamer;

  // An object that describes its instruction sites says how many prefix
  // bytes precede each indirect jump's opcode, since a linker that makes it
  // a direct jump rewrites it from its first byte. A prefix may be an
  // instruction of its own, so the streamer tracks what precedes each one:
  // where the last instruction ends, and the bytes of the run of prefixes
  // alone that ends with it, with nothing between them.
  MCFragment *LastInstFrag = nullptr;
  size_t LastInstEnd = 0;
  unsigned PrefixRun = 0;
  // Whether a label follows or splits that run, which makes what follows the
  // label reachable without all of it.
  bool LabelInPrefixRun = false;
  // A linker that finds a load nothing reads disposes of its bytes, which the
  // instruction directly after it may take as its own prefixes, so the
  // streamer measures that instruction for the last fused read described:
  // first the load is awaited, then what follows it.
  enum { NoFusedRead, AwaitingLoad, AwaitingNext } FusedRead = NoFusedRead;
  // A fused call is a call through a register after segment overrides that
  // pad it to six bytes, or REX.W jmp through a register followed by int3
  // that pads it to seven. The streamer marks where the call or jump ends once
  // it has seen it whole in that form: first the overrides and the call or
  // jump are awaited, then a jump's int3.
  enum { NoFusedCall, AwaitingCall, AwaitingInt3 } FusedCall = NoFusedCall;
  MCRegister FusedCallReg;
  unsigned FusedCallOverrides = 0;
  MCSymbol *FusedJumpEnd = nullptr;
  // A fused access, add or index is the next instruction, which the streamer
  // describes once it has seen that it is in a form the records describe.
  enum {
    NoFusedUse,
    AwaitingAccess,
    AwaitingAdd,
    AwaitingIndex
  } FusedUse = NoFusedUse;
  MCRegister FusedUseReg;
  // The next instruction is a jump that ends its function, which a linker may
  // leave out, so that what its function's length depends on is described.
  bool TailJump = false;

  bool continueFusedCall(const MCInst &Inst, bool Follows, int Preceding,
                         size_t Size);
  bool describeFusedUse(const MCInst &Inst, MCSymbol *Start,
                        ArrayRef<char> Bytes);
  void markResizable(MCFragment *Tail);

  bool followsLastInstruction() const;
  bool startsSection() const;
  void describeJumpPrefixes(MutableArrayRef<MCFixup> Fixups, int Preceding);

public:
  X86WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> AB,
                     std::unique_ptr<MCCodeEmitter> CE,
                     std::unique_ptr<MCObjectWriter> OW)
      : MCWinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW)) {
    if (getWriter().hasLinkRecords())
      MCII.reset(getTheX86_64Target().createMCInstrInfo());
  }

  void reset() override {
    LastInstFrag = nullptr;
    LastInstEnd = 0;
    PrefixRun = 0;
    LabelInPrefixRun = false;
    FusedRead = NoFusedRead;
    FusedCall = NoFusedCall;
    FusedJumpEnd = nullptr;
    FusedUse = NoFusedUse;
    TailJump = false;
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitCOFFLinkFusedCall(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedRead() override;
  void emitCOFFLinkFusedAccess(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedAdd(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedIndex(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkTailJump() override {
    MCWinCOFFStreamer::emitCOFFLinkTailJump();
    TailJump = true;
  }
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitWindowsUnwindTables(WinEH::FrameInfo *Frame) override;
  void emitWindowsUnwindTables() override;
  void emitCVFPOData(const MCSymbol *ProcSym, SMLoc Loc) override;
  void finishImpl() override;
};

// Whether what a fragment emits after its fixed contents is instructions:
// nothing, or padding with no-ops.
static bool hasInstructionTail(const MCFragment &F) {
  switch (F.getKind()) {
  case MCFragment::FT_Data:
  case MCFragment::FT_Nops:
  case MCFragment::FT_BoundaryAlign:
    return true;
  case MCFragment::FT_Align:
    return F.hasAlignEmitNops();
  default:
    return false;
  }
}

// Whether the bytes just before the current position are the last
// instruction's, or no-ops after it, so that no data lies between them.
bool X86WinCOFFStreamer::followsLastInstruction() const {
  MCFragment *F = getCurrentFragment();
  size_t Size = getCurFragSize();
  if (F == LastInstFrag)
    return Size == LastInstEnd;
  return Size == 0 && LastInstFrag && LastInstFrag->getNext() == F &&
         LastInstFrag->getFixedSize() == LastInstEnd &&
         hasInstructionTail(*LastInstFrag);
}

// Whether nothing but no-op padding precedes the current position in its
// section.
bool X86WinCOFFStreamer::startsSection() const {
  MCFragment *F = getCurrentFragment();
  if (getCurFragSize() != 0)
    return false;
  for (const MCFragment &Frag : *F->getParent()) {
    if (&Frag == F)
      return true;
    if (Frag.getFixedSize() != 0 || !hasInstructionTail(Frag))
      return false;
  }
  return false;
}

// Counts Preceding prefix bytes, or -1 for bytes that may be one but belong
// to no instruction, into the description of each jump among Fixups, which
// counts its own prefixes. A jump after more than one prefix byte, or after
// bytes that may be one, is not described as a jump.
void X86WinCOFFStreamer::describeJumpPrefixes(MutableArrayRef<MCFixup> Fixups,
                                              int Preceding) {
  for (MCFixup &Fixup : Fixups) {
    if (Fixup.getUse() != MCFixupUse::Jump)
      continue;
    unsigned Offset = Fixup.getInstOffset() + Preceding;
    if (Preceding < 0 || Offset > 3)
      Fixup.setUse(MCFixupUse::Unknown, 0);
    else
      Fixup.setUse(MCFixupUse::Jump, Offset);
  }
}

void X86WinCOFFStreamer::emitInstruction(const MCInst &Inst,
                                         const MCSubtargetInfo &STI) {
  if (!MCII) {
    X86_MC::emitInstruction(*this, Inst, STI);
    return;
  }

  // After the last instruction, prefixes alone are part of this one unless a
  // label separates them. Nothing precedes an instruction at the start of its
  // section. Other bytes, such as data, may be anything.
  MCFragment *F = getCurrentFragment();
  bool Follows = followsLastInstruction();
  int Preceding = 0;
  if (Follows) {
    if (PrefixRun)
      Preceding = LabelInPrefixRun ? -1 : PrefixRun;
  } else if (!startsSection()) {
    Preceding = -1;
  }

  // A fused access or add is labeled where it begins.
  MCSymbol *UseStart = nullptr;
  if (FusedUse != NoFusedUse) {
    UseStart = getContext().createTempSymbol();
    MCWinCOFFStreamer::emitLabel(UseStart);
  }

  size_t NumFixups = F->getFixups().size();
  size_t Start = getCurFragSize();
  X86_MC::emitInstruction(*this, Inst, STI);
  MCFragment *After = getCurrentFragment();
  // The instruction is in the fixed contents of the current fragment, after
  // any padding fragment the backend put before it, or, where the backend may
  // still pad or relax it, the variable tail of the fragment before.
  MCFragment *Tail = nullptr;
  for (MCFragment *Frag = F; Frag != After; Frag = Frag->getNext())
    Tail = Frag;
  MutableArrayRef<MCFixup> Fixups;
  size_t Size;
  ArrayRef<char> Bytes;
  if (Tail && Tail->getKind() == MCFragment::FT_Relaxable) {
    Fixups = Tail->getVarFixups();
    Size = Tail->getVarSize();
  } else {
    Tail = nullptr;
    Fixups = After->getFixups().drop_front(After == F ? NumFixups : 0);
    Size = getCurFragSize() - (After == F ? Start : 0);
    Bytes = After->getContents().take_back(Size);
  }
  describeJumpPrefixes(Fixups, Preceding);

  // A call or jump through an import pointer is one whose size a linker may
  // change, as is each fused instruction the streamer describes, and each jump
  // the backend may still relax, which the linker may shorten or lengthen
  // again once the code it spans is resized.
  bool Resizable = any_of(Fixups, [](const MCFixup &Fixup) {
    return Fixup.getUse() == MCFixupUse::Call ||
           Fixup.getUse() == MCFixupUse::Jump;
  });
  if (Tail && (Tail->getOpcode() == X86::JCC_1 ||
               Tail->getOpcode() == X86::JMP_1))
    Resizable = true;
  if (FusedUse != NoFusedUse) {
    checkFusedUse(Inst, FusedUseReg);
    if (Fixups.empty())
      Resizable |= describeFusedUse(Inst, UseStart, Bytes);
    FusedUse = NoFusedUse;
  }
  Resizable |= TailJump;
  TailJump = false;

  if (X86II::isPrefix(MCII->get(Inst.getOpcode()).TSFlags)) {
    // A prefix extends the run it follows. One whose bytes cannot be counted,
    // or that follows bytes that may be prefixes, leaves the run uncounted.
    if (!Follows || Preceding < 0) {
      PrefixRun = 0;
      LabelInPrefixRun = Preceding < 0;
    }
    PrefixRun += Size;
  } else {
    PrefixRun = 0;
    LabelInPrefixRun = false;
  }
  LastInstFrag = After;
  LastInstEnd = getCurFragSize();

  if (FusedCall != NoFusedCall)
    Resizable |= continueFusedCall(Inst, Follows, Preceding, Size);

  // The instruction after a fused read, with the prefixes before it, carries
  // the read's bytes only if nothing lies between them; one that transfers
  // control, or may fuse with a branch, then begins earlier, which the linker
  // weighs.
  if (FusedRead == AwaitingLoad) {
    FusedRead = AwaitingNext;
    Resizable = true;
  } else if (FusedRead == AwaitingNext) {
    bool IsPrefix = X86II::isPrefix(MCII->get(Inst.getOpcode()).TSFlags);
    if (!Follows || !IsPrefix)
      FusedRead = NoFusedRead;
    if (Follows && !IsPrefix) {
      const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
      MCSymbol *End = getContext().createTempSymbol();
      emitLabel(End);
      WinCOFFObjectWriter::LinkFusedRead &Read =
          getWriter().getLastLinkFusedRead();
      Read.NextEnd = End;
      Read.NextTransfers =
          Desc.isBranch() || Desc.isCall() || Desc.isReturn() ||
          X86::classifyFirstOpcodeInMacroFusion(Inst.getOpcode()) !=
              X86::FirstMacroFusionInstKind::Invalid;
    }
  }

  if (Resizable)
    markResizable(Tail);

  unsigned Opcode = Inst.getOpcode();
  noteInstruction(Inst, F, Start,
                  Opcode == X86::MOV64rr || Opcode == X86::MOV64rr_REV
                      ? Inst.getOperand(1).getReg()
                      : MCRegister());
}

// Marks the instruction just emitted, in Tail where the backend may still
// relax it and otherwise at the end of the current fragment, as one whose size
// a linker may change: it ends its fragment, so that the assembler leaves
// every distance across it to the linker, which the object's records then
// describe. Where the assembler pads branches, which depends on where each
// lies in the image, nothing is resized.
void X86WinCOFFStreamer::markResizable(MCFragment *Tail) {
  if (getAssembler().getBackend().allowAutoPadding())
    return;
  MCFragment *F = Tail ? Tail : getCurrentFragment();
  MCSection *Sec = F->getParent();
  if (!Sec->isLinkerRelaxable())
    Sec->setFirstLinkerRelaxable(F->getLayoutOrder());
  F->setLinkerRelaxable();
  if (Tail)
    return;
  // What follows the instruction directly begins the new fragment.
  newFragment();
  if (LastInstFrag == F) {
    LastInstFrag = getCurrentFragment();
    LastInstEnd = 0;
  }
}

// The register that the base of Inst's memory operand, at operand MemOp, is,
// with no index or segment and a displacement that needs no relocation.
static MCRegister getMemoryBase(const MCInst &Inst, unsigned MemOp) {
  const MCOperand &Disp = Inst.getOperand(MemOp + X86::AddrDisp);
  if (Inst.getOperand(MemOp + X86::AddrIndexReg).getReg() ||
      Inst.getOperand(MemOp + X86::AddrSegmentReg).getReg() || !Disp.isImm())
    return MCRegister();
  return Inst.getOperand(MemOp + X86::AddrBaseReg).getReg();
}

// Describes Inst, whose bytes are Bytes and which Start labels, as the fused
// access, add or index the streamer awaits, if it is in a form the records
// describe: an access with a legacy, VEX or EVEX encoding, outside APX's map 4,
// and no prefix that changes how its memory operand is addressed, whose memory
// operand has the register as its base with no index, and that reads the
// register nowhere else; an add of a constant to the register, or an lea of
// the register plus a constant; or an access with a legacy or VEX encoding
// whose memory operand adds the register, with scale 1, to another, as its
// base or its index, and that reads it nowhere else.
bool X86WinCOFFStreamer::describeFusedUse(const MCInst &Inst, MCSymbol *Start,
                                          ArrayRef<char> Bytes) {
  bool IsAccess = FusedUse != AwaitingAdd;
  bool IsIndex = FusedUse == AwaitingIndex;
  FusedUse = NoFusedUse;
  const MCInstrDesc &Desc = MCII->get(Inst.getOpcode());
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  unsigned Reg = MRI.getEncodingValue(FusedUseReg);
  if (Bytes.empty() || Reg >= 16)
    return false;

  if (!IsAccess) {
    WinCOFFObjectWriter::LinkFusedAdd &Add = getWriter().getLastLinkFusedAdd();
    MCRegister Dest;
    int64_t Constant;
    switch (Inst.getOpcode()) {
    case X86::ADD64ri8:
    case X86::ADD64ri32:
    case X86::SUB64ri8:
    case X86::SUB64ri32:
      if (Inst.getOperand(0).getReg() != FusedUseReg ||
          Inst.getOperand(1).getReg() != FusedUseReg ||
          !Inst.getOperand(2).isImm())
        return false;
      Dest = FusedUseReg;
      Constant = Inst.getOperand(2).getImm();
      if (Inst.getOpcode() == X86::SUB64ri8 ||
          Inst.getOpcode() == X86::SUB64ri32)
        Constant = -Constant;
      break;
    case X86::LEA64r:
      if (getMemoryBase(Inst, 1) != FusedUseReg)
        return false;
      Dest = Inst.getOperand(0).getReg();
      Constant = Inst.getOperand(1 + X86::AddrDisp).getImm();
      break;
    default:
      return false;
    }
    if (MRI.getEncodingValue(Dest) >= 16)
      return false;
    Add.Start = Start;
    Add.Register = MRI.getEncodingValue(Dest);
    Add.Constant = Constant;
    return true;
  }

  WinCOFFObjectWriter::LinkFusedAccess &Access =
      getWriter().getLastLinkFusedAccess();
  // An index may be an add of the register to another, or, of another to it:
  // REX.W, then 01 or 03, then ModRM naming both registers.
  if (IsIndex && (Inst.getOpcode() == X86::ADD64rr ||
                  Inst.getOpcode() == X86::ADD64rr_REV)) {
    MCRegister Dst = Inst.getOperand(0).getReg();
    MCRegister Src = Inst.getOperand(2).getReg();
    if ((Src != FusedUseReg && Dst != FusedUseReg) || Dst == Src ||
        MRI.getEncodingValue(Dst) >= 16 || MRI.getEncodingValue(Src) >= 16 ||
        Bytes.size() != 3 || (uint8_t(Bytes[0]) & 0xF8) != 0x48)
      return false;
    Access.Start = Start;
    Access.ModRM = 2;
    Access.Prefix = 0;
    Access.Length = 3;
    Access.Disp = 0;
    Access.SIB = Dst == FusedUseReg;
    Access.PrefixKind = COFF::LinkAccessREX;
    Access.Scale = 1;
    return true;
  }
  uint64_t Encoding = Desc.TSFlags & X86II::EncodingMask;
  uint64_t Map = Desc.TSFlags & X86II::OpMapMask;
  if ((Encoding != X86II::LEGACY && Encoding != X86II::VEX &&
       (Encoding != X86II::EVEX || IsIndex)) ||
      Map == X86II::T_MAP4 || Bytes.size() > 15)
    return false;
  int MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
  if (MemOp < 0)
    return false;
  MemOp += X86II::getOperandBias(Desc);
  // An index is the other register of the operand, which the linker keeps as
  // its base.
  bool IsBase = false;
  if (IsIndex) {
    MCRegister Base = Inst.getOperand(MemOp + X86::AddrBaseReg).getReg();
    MCRegister Index = Inst.getOperand(MemOp + X86::AddrIndexReg).getReg();
    if (Inst.getOperand(MemOp + X86::AddrScaleAmt).getImm() != 1 ||
        Inst.getOperand(MemOp + X86::AddrSegmentReg).getReg() ||
        !Inst.getOperand(MemOp + X86::AddrDisp).isImm() || !Base || !Index ||
        Base == X86::RIP)
      return false;
    IsBase = Base == FusedUseReg;
    MCRegister Other = IsBase ? Index : Base;
    if ((!IsBase && Index != FusedUseReg) ||
        MRI.regsOverlap(Other, FusedUseReg) ||
        MRI.getEncodingValue(Other) >= 16)
      return false;
  } else if (getMemoryBase(Inst, MemOp) != FusedUseReg) {
    return false;
  }
  // Nothing else may read the register, which the linker leaves unfilled.
  for (unsigned I = 0, E = Inst.getNumOperands(); I != E; ++I) {
    const MCOperand &MO = Inst.getOperand(I);
    if (I >= unsigned(MemOp) && I < unsigned(MemOp) + X86::AddrNumOperands)
      continue;
    if (MO.isReg() && MO.getReg() &&
        MRI.regsOverlap(MO.getReg(), FusedUseReg) &&
        (I >= Desc.getNumDefs() ||
         Desc.getOperandConstraint(I, MCOI::TIED_TO) != -1))
      return false;
  }
  if (any_of(Desc.implicit_uses(),
             [&](MCPhysReg R) { return MRI.regsOverlap(R, FusedUseReg); }))
    return false;

  // The encoder's bytes: prefixes that do not change addressing, then REX
  // and the opcode in its map, or VEX or EVEX and the opcode, then ModRM,
  // which must address memory through the register, with a SIB byte that
  // names no index, or for an index the register and the other one. The base
  // register's high bit is REX.B, or VEX's or EVEX's inverted B, and the
  // index's REX.X or the inverted X; two-byte VEX holds neither, its
  // registers being among the first eight.
  size_t I = 0;
  while (I < Bytes.size() &&
         is_contained({0x66, 0xF0, 0xF2, 0xF3}, uint8_t(Bytes[I])))
    ++I;
  unsigned Prefix = 15, HighBit = 0, IndexHighBit = 0;
  uint8_t PrefixKind = COFF::LinkAccessREX;
  unsigned ModRMOffset;
  if (Encoding == X86II::LEGACY) {
    if (I < Bytes.size() && (uint8_t(Bytes[I]) & 0xF0) == 0x40) {
      Prefix = I;
      HighBit = Bytes[I] & 1;
      IndexHighBit = Bytes[I] >> 1 & 1;
      if (IndexHighBit && !IsIndex)
        return false;
      ++I;
    }
    switch (Map) {
    case X86II::OB:
      break;
    case X86II::TB:
      I += 1;
      break;
    case X86II::T8:
    case X86II::TA:
      I += 2;
      break;
    default:
      return false;
    }
    ModRMOffset = I + 1;
  } else {
    if (I + 2 >= Bytes.size())
      return false;
    uint8_t Lead = Bytes[I];
    if (Lead == 0xC5) {
      ModRMOffset = I + 3;
    } else if (Lead == 0xC4 || Lead == 0x62) {
      // ~X must be set unless the register is an index.
      IndexHighBit = !(Bytes[I + 1] & 0x40);
      if (IndexHighBit && !IsIndex)
        return false;
      Prefix = I;
      PrefixKind = Lead == 0xC4 ? COFF::LinkAccessVEX : COFF::LinkAccessEVEX;
      HighBit = !(Bytes[I + 1] & 0x20);
      ModRMOffset = I + (Lead == 0xC4 ? 4 : 5);
    } else {
      return false;
    }
  }
  if (ModRMOffset >= Bytes.size())
    return false;
  uint8_t ModRM = Bytes[ModRMOffset];
  unsigned Mod = ModRM >> 6, RM = ModRM & 7, Base = RM, Index = 4;
  bool SIB = RM == 4;
  if (Mod == 3 || (Mod == 0 && RM == 5) || (IsIndex && !SIB))
    return false;
  if (SIB) {
    if (ModRMOffset + 1 >= Bytes.size())
      return false;
    uint8_t SIBByte = Bytes[ModRMOffset + 1];
    Base = SIBByte & 7;
    Index = SIBByte >> 3 & 7;
    // An index field of 4 names no index unless its high bit makes it r12.
    if ((Index != 4 || IndexHighBit) != IsIndex || (Mod == 0 && Base == 5) ||
        (IsIndex && SIBByte >> 6))
      return false;
  }
  if (IsIndex ? (IsBase ? (Base | HighBit << 3)
                        : (Index | IndexHighBit << 3)) != Reg
              : (Base | HighBit << 3) != Reg)
    return false;
  // EVEX multiplies an 8-bit displacement by a scale the instruction
  // implies, which the operand's displacement and the byte give.
  unsigned Scale = 0;
  if (PrefixKind == COFF::LinkAccessEVEX && Mod == 1) {
    int8_t Disp8 = Bytes[ModRMOffset + 1 + SIB];
    int64_t Disp = Inst.getOperand(MemOp + X86::AddrDisp).getImm();
    if (Disp8 != 0) {
      if (Disp % Disp8 || !isPowerOf2_64(Disp / Disp8) || Disp / Disp8 > 64)
        return false;
      Scale = Log2_64(Disp / Disp8);
    }
  }
  Access.Start = Start;
  Access.ModRM = ModRMOffset;
  Access.Prefix = Prefix;
  Access.Length = Bytes.size();
  Access.Disp = Mod == 1 ? 1 : Mod == 2 ? 2 : 0;
  Access.SIB = IsIndex ? IsBase : SIB;
  Access.PrefixKind = PrefixKind;
  Access.Scale = Scale;
  return true;
}

// Takes the next instruction of a fused call, whose description is dropped
// unless the call or jump is in the form it describes, all its bytes
// following one another: overrides that only it reads and a call that end six
// bytes after the first override, or a jump of three bytes with REX.W, which
// assembly gives as rex64, that int3 follows to seven. A jump's region begins
// with the jump, wherever padding puts it.
bool X86WinCOFFStreamer::continueFusedCall(const MCInst &Inst, bool Follows,
                                           int Preceding, size_t Size) {
  unsigned Opcode = Inst.getOpcode();
  WinCOFFObjectWriter::LinkFusedCall &Call = getWriter().getLastLinkFusedCall();
  if (FusedCall == AwaitingInt3) {
    if (Opcode != X86::INT3 || !Follows) {
      FusedCall = NoFusedCall;
    } else if (++FusedCallOverrides == 4) {
      FusedCall = NoFusedCall;
      Call.End = FusedJumpEnd;
      Call.IsJump = true;
      return true;
    }
    return false;
  }
  if (Opcode == X86::CS_PREFIX) {
    ++FusedCallOverrides;
    return false;
  }
  FusedCall = NoFusedCall;
  checkFusedUse(Inst, FusedCallReg);
  bool ThroughReg = Inst.getNumOperands() && Inst.getOperand(0).isReg() &&
                    Inst.getOperand(0).getReg() == FusedCallReg;
  if (!ThroughReg || Preceding != int(FusedCallOverrides))
    return false;
  bool HasREXW = Inst.getFlags() & X86::IP_HAS_REX_W;
  bool IsCall = Opcode == X86::CALL64r;
  bool IsJump = !FusedCallOverrides && (Opcode == X86::JMP64r_REX ||
                                        (Opcode == X86::JMP64r && HasREXW));
  if ((IsCall && FusedCallOverrides + Size == 6) || (IsJump && Size == 3)) {
    MCSymbol *End = getContext().createTempSymbol();
    emitLabel(End);
    if (IsCall) {
      Call.End = End;
      return true;
    }
    FusedJumpEnd = End;
    FusedCallOverrides = 0;
    FusedCall = AwaitingInt3;
  }
  return false;
}

void X86WinCOFFStreamer::emitCOFFLinkFusedCall(const MCSymbol *Load,
                                               MCRegister Reg) {
  MCWinCOFFStreamer::emitCOFFLinkFusedCall(Load, Reg);
  if (!MCII)
    return;
  FusedCall = AwaitingCall;
  FusedCallReg = Reg;
  FusedCallOverrides = 0;
}

void X86WinCOFFStreamer::emitCOFFLinkFusedAccess(const MCSymbol *Load,
                                                 MCRegister Reg) {
  MCWinCOFFStreamer::emitCOFFLinkFusedAccess(Load, Reg);
  if (!MCII)
    return;
  FusedUse = AwaitingAccess;
  FusedUseReg = Reg;
}

void X86WinCOFFStreamer::emitCOFFLinkFusedAdd(const MCSymbol *Load,
                                              MCRegister Reg) {
  MCWinCOFFStreamer::emitCOFFLinkFusedAdd(Load, Reg);
  if (!MCII)
    return;
  FusedUse = AwaitingAdd;
  FusedUseReg = Reg;
}

void X86WinCOFFStreamer::emitCOFFLinkFusedIndex(const MCSymbol *Load,
                                                MCRegister Reg) {
  MCWinCOFFStreamer::emitCOFFLinkFusedIndex(Load, Reg);
  if (!MCII)
    return;
  FusedUse = AwaitingIndex;
  FusedUseReg = Reg;
}

// A fused read that directly follows another leaves the first without an
// instruction to carry its bytes.
void X86WinCOFFStreamer::emitCOFFLinkFusedRead() {
  MCWinCOFFStreamer::emitCOFFLinkFusedRead();
  if (MCII)
    FusedRead = AwaitingLoad;
}

void X86WinCOFFStreamer::emitLabel(MCSymbol *Symbol, SMLoc Loc) {
  MCWinCOFFStreamer::emitLabel(Symbol, Loc);
  if (PrefixRun)
    LabelInPrefixRun = true;
}

void X86WinCOFFStreamer::emitWinEHHandlerData(SMLoc Loc) {
  MCStreamer::emitWinEHHandlerData(Loc);

  // We have to emit the unwind info now, because this directive
  // actually switches to the .xdata section.
  if (WinEH::FrameInfo *CurFrame = getCurrentWinFrameInfo()) {
    // Handlers are always associated with the parent frame.
    CurFrame = CurFrame->ChainedParent ? CurFrame->ChainedParent : CurFrame;
    EHStreamer.EmitUnwindInfo(*this, CurFrame, /* HandlerData = */ true);
  }
}

void X86WinCOFFStreamer::emitWindowsUnwindTables(WinEH::FrameInfo *Frame) {
  EHStreamer.EmitUnwindInfo(*this, Frame, /* HandlerData = */ false);
}

void X86WinCOFFStreamer::emitWindowsUnwindTables() {
  if (!getNumWinFrameInfos())
    return;
  EHStreamer.Emit(*this);
}

void X86WinCOFFStreamer::emitCVFPOData(const MCSymbol *ProcSym, SMLoc Loc) {
  X86TargetStreamer *XTS =
      static_cast<X86TargetStreamer *>(getTargetStreamer());
  XTS->emitFPOData(ProcSym, Loc);
}

void X86WinCOFFStreamer::finishImpl() {
  emitFrames();
  emitWindowsUnwindTables();

  MCWinCOFFStreamer::finishImpl();
}
} // namespace

MCStreamer *
llvm::createX86WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> &&AB,
                               std::unique_ptr<MCObjectWriter> &&OW,
                               std::unique_ptr<MCCodeEmitter> &&CE) {
  return new X86WinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW));
}
