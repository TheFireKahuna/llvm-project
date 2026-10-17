//===-- X86WinCOFFStreamer.cpp - X86 Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86MCTargetDesc.h"
#include "TargetInfo/X86TargetInfo.h"
#include "X86BaseInfo.h"
#include "X86TargetStreamer.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectWriter.h"
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
  // instruction of its own, so the streamer tracks what precedes each one.
  std::unique_ptr<const MCInstrInfo> MCII;
  // Where the last instruction ends, and the bytes of the run of prefixes
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

  void continueFusedCall(const MCInst &Inst, bool Follows, int Preceding,
                         size_t Size);

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
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitCOFFLinkFusedCall(const MCSymbol *Load, MCRegister Reg) override;
  void emitCOFFLinkFusedRead() override;
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
  if (Tail && Tail->getKind() == MCFragment::FT_Relaxable) {
    Fixups = Tail->getVarFixups();
    Size = Tail->getVarSize();
  } else {
    Fixups = After->getFixups().drop_front(After == F ? NumFixups : 0);
    Size = getCurFragSize() - (After == F ? Start : 0);
  }
  describeJumpPrefixes(Fixups, Preceding);

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
    continueFusedCall(Inst, Follows, Preceding, Size);

  // The instruction after a fused read, with the prefixes before it, carries
  // the read's bytes only if nothing lies between them; one that transfers
  // control, or may fuse with a branch, then begins earlier, which the linker
  // weighs.
  if (FusedRead == AwaitingLoad) {
    FusedRead = AwaitingNext;
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
}

// Takes the next instruction of a fused call, whose description is dropped
// unless the call or jump is in the form it describes, all its bytes
// following one another: overrides that only it reads and a call that end six
// bytes after the first override, or a jump of three bytes with REX.W, which
// assembly gives as rex64, that int3 follows to seven. A jump's region begins
// with the jump, wherever padding puts it.
void X86WinCOFFStreamer::continueFusedCall(const MCInst &Inst, bool Follows,
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
    }
    return;
  }
  if (Opcode == X86::CS_PREFIX) {
    ++FusedCallOverrides;
    return;
  }
  FusedCall = NoFusedCall;
  bool ThroughReg = Inst.getNumOperands() && Inst.getOperand(0).isReg() &&
                    Inst.getOperand(0).getReg() == FusedCallReg;
  if (!ThroughReg || Preceding != int(FusedCallOverrides))
    return;
  bool HasREXW = Inst.getFlags() & X86::IP_HAS_REX_W;
  bool IsCall = Opcode == X86::CALL64r;
  bool IsJump = !FusedCallOverrides && (Opcode == X86::JMP64r_REX ||
                                        (Opcode == X86::JMP64r && HasREXW));
  if ((IsCall && FusedCallOverrides + Size == 6) || (IsJump && Size == 3)) {
    MCSymbol *End = getContext().createTempSymbol();
    emitLabel(End);
    if (IsCall) {
      Call.End = End;
    } else {
      FusedJumpEnd = End;
      FusedCallOverrides = 0;
      FusedCall = AwaitingInt3;
    }
  }
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
