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
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
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
