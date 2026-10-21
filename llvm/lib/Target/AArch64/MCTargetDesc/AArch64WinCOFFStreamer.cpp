//===-- AArch64WinCOFFStreamer.cpp - ARM Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AArch64WinCOFFStreamer.h"
#include "AArch64AddressingModes.h"
#include "AArch64MCTargetDesc.h"
#include "TargetInfo/AArch64TargetInfo.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCWin64EH.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace {

class AArch64WinCOFFStreamer : public MCWinCOFFStreamer {
  Win64EH::ARM64UnwindEmitter EHStreamer;
  // A fused access or add is the next instruction, which the streamer
  // describes once it has seen that it is in a form the records describe.
  enum {
    NoFusedUse,
    AwaitingAccess,
    AwaitingAdd,
    AwaitingIndex
  } FusedUse = NoFusedUse;
  MCRegister FusedUseReg;

  bool describeFusedUse(const MCInst &Inst, MCSymbol *Start);

public:
  AArch64WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> AB,
                         std::unique_ptr<MCCodeEmitter> CE,
                         std::unique_ptr<MCObjectWriter> OW)
      : MCWinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW)) {
    if (getWriter().hasLinkRecords())
      MCII.reset(getTheAArch64leTarget().createMCInstrInfo());
  }

  void reset() override {
    FusedUse = NoFusedUse;
    MCWinCOFFStreamer::reset();
  }
  void emitInstruction(const MCInst &Inst,
                       const MCSubtargetInfo &STI) override {
    if (FusedUse == NoFusedUse && !MCII) {
      MCWinCOFFStreamer::emitInstruction(Inst, STI);
      return;
    }
    // A fused access or add is labeled where it begins.
    MCSymbol *Start = nullptr;
    if (FusedUse != NoFusedUse) {
      Start = getContext().createTempSymbol();
      emitLabel(Start);
    }
    const MCFragment *F = getCurrentFragment();
    uint64_t Offset = getCurFragSize();
    MCWinCOFFStreamer::emitInstruction(Inst, STI);
    if (Start) {
      if (MCII)
        checkFusedUse(Inst, FusedUseReg);
      describeFusedUse(Inst, Start);
      FusedUse = NoFusedUse;
    }
    // A copy is orr of the register to the zero register, unshifted.
    if (MCII)
      noteInstruction(Inst, F, Offset,
                      Inst.getOpcode() == AArch64::ORRXrs &&
                              Inst.getOperand(1).getReg() == AArch64::XZR &&
                              !Inst.getOperand(3).getImm()
                          ? Inst.getOperand(2).getReg()
                          : MCRegister());
  }
  void emitCOFFLinkFusedAccess(const MCSymbol *Load, MCRegister Reg) override {
    MCWinCOFFStreamer::emitCOFFLinkFusedAccess(Load, Reg);
    FusedUse = AwaitingAccess;
    FusedUseReg = Reg;
  }
  void emitCOFFLinkFusedAdd(const MCSymbol *Load, MCRegister Reg) override {
    MCWinCOFFStreamer::emitCOFFLinkFusedAdd(Load, Reg);
    FusedUse = AwaitingAdd;
    FusedUseReg = Reg;
  }
  void emitCOFFLinkFusedIndex(const MCSymbol *Load, MCRegister Reg) override {
    MCWinCOFFStreamer::emitCOFFLinkFusedIndex(Load, Reg);
    FusedUse = AwaitingIndex;
    FusedUseReg = Reg;
  }

  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitWindowsUnwindTables() override;
  void emitWindowsUnwindTables(WinEH::FrameInfo *Frame) override;
  void finishImpl() override;
};

// Describes Inst, which Start labels, as the fused access or add the streamer
// awaits, if it is in a form the records describe: a load, store or prefetch
// at an unsigned offset from the register, or a pair of loads or stores at a
// signed one, reading it nowhere else; or an add or sub of an immediate to the
// register.
bool AArch64WinCOFFStreamer::describeFusedUse(const MCInst &Inst,
                                              MCSymbol *Start) {
  const MCRegisterInfo &MRI = *getContext().getRegisterInfo();
  // An index, a load, store or prefetch at the sum of two registers or an
  // add of them, names the register as its first (base) or second; the
  // linker decodes the rest.
  if (FusedUse == AwaitingIndex) {
    if (Inst.getNumOperands() < 3 || !Inst.getOperand(1).isReg() ||
        !Inst.getOperand(2).isReg())
      return false;
    bool IsBase = Inst.getOperand(1).getReg() == FusedUseReg;
    if (!IsBase && Inst.getOperand(2).getReg() != FusedUseReg)
      return false;
    WinCOFFObjectWriter::LinkFusedAccess &Access =
        getWriter().getLastLinkFusedAccess();
    Access.Start = Start;
    Access.SIB = IsBase;
    return true;
  }
  if (FusedUse == AwaitingAdd) {
    unsigned Opcode = Inst.getOpcode();
    if ((Opcode != AArch64::ADDXri && Opcode != AArch64::SUBXri) ||
        Inst.getOperand(1).getReg() != FusedUseReg ||
        !Inst.getOperand(2).isImm() || !Inst.getOperand(3).isImm())
      return false;
    int64_t Constant = Inst.getOperand(2).getImm()
                       << AArch64_AM::getShiftValue(Inst.getOperand(3).getImm());
    WinCOFFObjectWriter::LinkFusedAdd &Add = getWriter().getLastLinkFusedAdd();
    Add.Start = Start;
    Add.Register = MRI.getEncodingValue(Inst.getOperand(0).getReg());
    Add.Constant = Opcode == AArch64::SUBXri ? -Constant : Constant;
    return true;
  }
  unsigned Base;
  switch (Inst.getOpcode()) {
  case AArch64::LDRBBui:
  case AArch64::LDRHHui:
  case AArch64::LDRWui:
  case AArch64::LDRXui:
  case AArch64::LDRSBWui:
  case AArch64::LDRSBXui:
  case AArch64::LDRSHWui:
  case AArch64::LDRSHXui:
  case AArch64::LDRSWui:
  case AArch64::LDRBui:
  case AArch64::LDRHui:
  case AArch64::LDRSui:
  case AArch64::LDRDui:
  case AArch64::LDRQui:
  case AArch64::PRFMui:
    Base = 1;
    break;
  case AArch64::LDPWi:
  case AArch64::LDPXi:
  case AArch64::LDPSWi:
  case AArch64::LDPSi:
  case AArch64::LDPDi:
  case AArch64::LDPQi:
    Base = 2;
    break;
  case AArch64::STRBBui:
  case AArch64::STRHHui:
  case AArch64::STRWui:
  case AArch64::STRXui:
  case AArch64::STRBui:
  case AArch64::STRHui:
  case AArch64::STRSui:
  case AArch64::STRDui:
  case AArch64::STRQui:
    Base = 1;
    // A store of the register itself would store the page it holds instead.
    if (MRI.regsOverlap(Inst.getOperand(0).getReg(), FusedUseReg))
      return false;
    break;
  case AArch64::STPWi:
  case AArch64::STPXi:
  case AArch64::STPSi:
  case AArch64::STPDi:
  case AArch64::STPQi:
    Base = 2;
    if (MRI.regsOverlap(Inst.getOperand(0).getReg(), FusedUseReg) ||
        MRI.regsOverlap(Inst.getOperand(1).getReg(), FusedUseReg))
      return false;
    break;
  default:
    return false;
  }
  if (Inst.getOperand(Base).getReg() != FusedUseReg ||
      !Inst.getOperand(Base + 1).isImm())
    return false;
  getWriter().getLastLinkFusedAccess().Start = Start;
  return true;
}

void AArch64WinCOFFStreamer::emitWinEHHandlerData(SMLoc Loc) {
  MCStreamer::emitWinEHHandlerData(Loc);

  // We have to emit the unwind info now, because this directive
  // actually switches to the .xdata section!
  EHStreamer.EmitUnwindInfo(*this, getCurrentWinFrameInfo(),
                            /* HandlerData = */ true);
}

void AArch64WinCOFFStreamer::emitWindowsUnwindTables(WinEH::FrameInfo *Frame) {
  EHStreamer.EmitUnwindInfo(*this, Frame, /* HandlerData = */ false);
}

void AArch64WinCOFFStreamer::emitWindowsUnwindTables() {
  if (!getNumWinFrameInfos())
    return;
  EHStreamer.Emit(*this);
}

void AArch64WinCOFFStreamer::finishImpl() {
  emitFrames();
  emitWindowsUnwindTables();

  MCWinCOFFStreamer::finishImpl();
}
} // end anonymous namespace

// Helper function to common out unwind code setup for those codes that can
// belong to both prolog and epilog.
// There are three types of Windows ARM64 SEH codes.  They can
// 1) take no operands: SEH_Nop, SEH_PrologEnd, SEH_EpilogStart, SEH_EpilogEnd
// 2) take an offset: SEH_StackAlloc, SEH_SaveFPLR, SEH_SaveFPLR_X
// 3) take a register and an offset/size: all others
void AArch64TargetWinCOFFStreamer::emitARM64WinUnwindCode(unsigned UnwindCode,
                                                          int Reg, int Offset) {
  auto &S = getStreamer();
  WinEH::FrameInfo *CurFrame = S.EnsureValidWinFrameInfo(SMLoc());
  if (!CurFrame)
    return;
  auto Inst = WinEH::Instruction(UnwindCode, /*Label=*/nullptr, Reg, Offset);
  if (S.isInEpilogCFI())
    S.getCurrentWinEpilog()->Instructions.push_back(Inst);
  else
    CurFrame->Instructions.push_back(Inst);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIAllocStack(unsigned Size) {
  unsigned Op = Win64EH::UOP_AllocSmall;
  if (Size >= 16384)
    Op = Win64EH::UOP_AllocLarge;
  else if (Size >= 512)
    Op = Win64EH::UOP_AllocMedium;
  emitARM64WinUnwindCode(Op, -1, Size);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveR19R20X(int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveR19R20X, -1, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFPLR(int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFPLR, -1, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFPLRX(int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFPLRX, -1, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveReg(unsigned Reg,
                                                          int Offset) {
  assert(Offset >= 0 && Offset <= 504 &&
        "Offset for save reg should be >= 0 && <= 504");
  emitARM64WinUnwindCode(Win64EH::UOP_SaveReg, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveRegX(unsigned Reg,
                                                           int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveRegX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveRegP(unsigned Reg,
                                                           int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveRegP, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveRegPX(unsigned Reg,
                                                            int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveRegPX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveLRPair(unsigned Reg,
                                                             int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveLRPair, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFReg(unsigned Reg,
                                                           int Offset) {
  assert(Offset >= 0 && Offset <= 504 &&
        "Offset for save reg should be >= 0 && <= 504");
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFReg, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFRegX(unsigned Reg,
                                                            int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFRegX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFRegP(unsigned Reg,
                                                            int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFRegP, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveFRegPX(unsigned Reg,
                                                             int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveFRegPX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISetFP() {
  emitARM64WinUnwindCode(Win64EH::UOP_SetFP, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIAddFP(unsigned Offset) {
  assert(Offset <= 2040 && "UOP_AddFP must have offset <= 2040");
  emitARM64WinUnwindCode(Win64EH::UOP_AddFP, -1, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFINop() {
  emitARM64WinUnwindCode(Win64EH::UOP_Nop, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveNext() {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveNext, -1, 0);
}

// The functions below handle opcodes that can end up in either a prolog or
// an epilog, but not both.
void AArch64TargetWinCOFFStreamer::emitARM64WinCFIPrologEnd() {
  auto &S = getStreamer();
  WinEH::FrameInfo *CurFrame = S.EnsureValidWinFrameInfo(SMLoc());
  if (!CurFrame)
    return;

  MCSymbol *Label = S.emitCFILabel();
  CurFrame->PrologEnd = Label;
  WinEH::Instruction Inst =
      WinEH::Instruction(Win64EH::UOP_End, /*Label=*/nullptr, -1, 0);
  auto it = CurFrame->Instructions.begin();
  CurFrame->Instructions.insert(it, Inst);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIEpilogStart() {
  getStreamer().emitWinCFIBeginEpilogue();
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIEpilogEnd() {
  auto &S = getStreamer();
  WinEH::FrameInfo *CurFrame = S.EnsureValidWinFrameInfo(SMLoc());
  if (!CurFrame)
    return;

  if (S.isInEpilogCFI()) {
    WinEH::Instruction Inst =
        WinEH::Instruction(Win64EH::UOP_End, /*Label=*/nullptr, -1, 0);
    S.getCurrentWinEpilog()->Instructions.push_back(Inst);
  }
  S.emitWinCFIEndEpilogue();
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFITrapFrame() {
  emitARM64WinUnwindCode(Win64EH::UOP_TrapFrame, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIMachineFrame() {
  emitARM64WinUnwindCode(Win64EH::UOP_PushMachFrame, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIContext() {
  emitARM64WinUnwindCode(Win64EH::UOP_Context, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIECContext() {
  emitARM64WinUnwindCode(Win64EH::UOP_ECContext, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIClearUnwoundToCall() {
  emitARM64WinUnwindCode(Win64EH::UOP_ClearUnwoundToCall, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIPACSignLR() {
  emitARM64WinUnwindCode(Win64EH::UOP_PACSignLR, -1, 0);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegI(unsigned Reg,
                                                              int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegI, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegIP(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegIP, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegD(unsigned Reg,
                                                              int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegD, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegDP(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegDP, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegQ(unsigned Reg,
                                                              int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegQ, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegQP(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegQP, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegIX(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegIX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegIPX(unsigned Reg,
                                                                int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegIPX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegDX(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegDX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegDPX(unsigned Reg,
                                                                int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegDPX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegQX(unsigned Reg,
                                                               int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegQX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveAnyRegQPX(unsigned Reg,
                                                                int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveAnyRegQPX, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIAllocZ(int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_AllocZ, 0, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISaveZReg(unsigned Reg,
                                                           int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SaveZReg, Reg, Offset);
}

void AArch64TargetWinCOFFStreamer::emitARM64WinCFISavePReg(unsigned Reg,
                                                           int Offset) {
  emitARM64WinUnwindCode(Win64EH::UOP_SavePReg, Reg, Offset);
}

MCStreamer *
llvm::createAArch64WinCOFFStreamer(MCContext &Context,
                                   std::unique_ptr<MCAsmBackend> &&MAB,
                                   std::unique_ptr<MCObjectWriter> &&OW,
                                   std::unique_ptr<MCCodeEmitter> &&Emitter) {
  return new AArch64WinCOFFStreamer(Context, std::move(MAB), std::move(Emitter),
                                    std::move(OW));
}
