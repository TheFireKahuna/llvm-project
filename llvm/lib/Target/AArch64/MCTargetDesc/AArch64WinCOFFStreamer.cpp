//===-- AArch64WinCOFFStreamer.cpp - ARM Target WinCOFF Streamer ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AArch64WinCOFFStreamer.h"
#include "AArch64MCAsmInfo.h"
#include "AArch64MCTargetDesc.h"
#include "TargetInfo/AArch64TargetInfo.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCWin64EH.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

namespace {

class AArch64WinCOFFStreamer : public MCWinCOFFStreamer {
  Win64EH::ARM64UnwindEmitter EHStreamer;
  // The fragment, the word and the relocation type of an adrp, or of the add
  // of a thread-local's high part or of a low part that has none, that its
  // section's last instruction is, which the linker may fold or delete with
  // the next.
  std::tuple<MCFragment *, uint32_t, uint16_t> First;

public:
  AArch64WinCOFFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> AB,
                         std::unique_ptr<MCCodeEmitter> CE,
                         std::unique_ptr<MCObjectWriter> OW)
      : MCWinCOFFStreamer(C, std::move(AB), std::move(CE), std::move(OW)) {
    if (C.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
      MCII.reset(getTheAArch64leTarget().createMCInstrInfo());
      // The wide form: the place's adrp and ldr, the word's ldr, the key's
      // adrp and ldr, and the eor.
      TlsBaseInsts = 6;
      FusedForms[size_t(COFF::FusedUse::Call)] = "blr";
      FusedForms[size_t(COFF::FusedUse::Jump)] = "br";
      FusedForms[size_t(COFF::FusedUse::Access)] =
          "load or store with an offset that is a number";
      FusedForms[size_t(COFF::FusedUse::Index)] =
          "use of the word of a record that its fact states";
      FusedForms[size_t(COFF::FusedUse::Test)] =
          "cbz or cbnz of a 64-bit register";
      FusedForms[size_t(COFF::FusedUse::Add)] =
          "64-bit add of an unshifted immediate";
    }
  }

  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitValueImpl(const MCExpr *Value, unsigned Size, SMLoc Loc) override;
  void emitWinEHHandlerData(SMLoc Loc) override;
  void emitWindowsUnwindTables() override;
  void emitWindowsUnwindTables(WinEH::FrameInfo *Frame) override;
  void finishImpl() override;
};

void AArch64WinCOFFStreamer::emitInstruction(const MCInst &Inst,
                                             const MCSubtargetInfo &STI) {
  // A section that holds an instruction is aligned as one, wherever the
  // linker lays it.
  getCurrentSectionOnly()->ensureMinAlignment(Align(4));
  // The code emitter states the instructions that the linker may resize but
  // a pair, which the list states by its two words; a fused jump that
  // the linker may delete has no relocation of the emitter's.
  bool Tail = false;
  scope_exit Emitted([&] {
    if (!MCII)
      return;
    MCFragment *F = getCurrentFragment();
    ArrayRef<char> Code = F->getContents();
    uint32_t Word =
        Code.size() < 4 ? 0 : support::endian::read32le(Code.end() - 4);
    auto [FirstF, FirstWord, Type] = First;
    bool After = FirstF && FirstF->getParent() == F->getParent();
    bool Fold = After && COFF::relocationMayResizeCode(
                             COFF::IMAGE_FILE_MACHINE_ARM64, Type, {},
                             FirstWord | uint64_t(Word) << 32);
    finishInstruction(Tail || Fold, Fold ? FirstF : nullptr);
    if (After)
      First = {};
    auto *Part =
        Inst.getOpcode() == AArch64::ADDXri && Inst.getOperand(2).isExpr()
            ? dyn_cast<MCSpecifierExpr>(Inst.getOperand(2).getExpr())
            : nullptr;
    if (Inst.getOpcode() == AArch64::ADRP)
      First = {F, Word, COFF::IMAGE_REL_ARM64_PAGEBASE_REL21};
    else if (Part && Part->getSpecifier() == AArch64::S_SECREL_HI12)
      First = {F, Word, COFF::IMAGE_REL_ARM64_SECREL_HIGH12A};
    else if (Part && Part->getSpecifier() == AArch64::S_TPREL_HI12)
      First = {F, Word, COFF::IMAGE_REL_LLVM_TPOFF_HIGH12A};
    else if (Part && Part->getSpecifier() == AArch64::S_DTPREL_LO12)
      First = {F, Word, COFF::IMAGE_REL_LLVM_DTPOFF_LOW12A};
    else if (Part && Part->getSpecifier() == AArch64::S_TPREL_LO12)
      First = {F, Word, COFF::IMAGE_REL_LLVM_TPOFF_LOW12A};
  });
  if (MCII) {
    // A 64-bit ldr of a pointer loads it, and is a fused read where its
    // relocation states so.
    const MCSymbol *Load = nullptr;
    bool Read = false;
    for (const MCOperand &MO : Inst) {
      auto *SE =
          MO.isExpr() ? dyn_cast<MCSpecifierExpr>(MO.getExpr()) : nullptr;
      if (!SE || (SE->getSpecifier() != AArch64::S_FUSED_LO12 &&
                  SE->getSpecifier() != AArch64::S_LO12))
        continue;
      auto *Pointer = dyn_cast<MCSymbolRefExpr>(SE->getSubExpr());
      Read = SE->getSpecifier() == AArch64::S_FUSED_LO12;
      if (Inst.getOpcode() == AArch64::LDRXui && Pointer)
        Load = &Pointer->getSymbol();
      else if (Read)
        getContext().reportError(Inst.getLoc(),
                                 "fused read is not a 64-bit ldr of a pointer");
    }
    // A br that no unwind frame covers may be deleted, as a b there may.
    unsigned RegOp;
    COFF::FusedUse Use = AArch64_MC::getFusedUse(Inst, RegOp);
    selectFusedIndex(Inst, Use, RegOp);
    Tail = FusedSite.Offset && FusedSite.Use == COFF::FusedUse::Jump &&
           Use == FusedSite.Use && !getCurrentWinFrameInfo();
    checkFused(Inst, Use, RegOp, Load, Read, Tail);
  }

  // On Windows Itanium and NT-POSIX the relocation type of a b states whether
  // the linker may delete it, which it may where no unwind frame covers it.
  if (Inst.getOpcode() == AArch64::B && !getCurrentWinFrameInfo() &&
      STI.getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment()) {
    MCInst Tail(Inst);
    Tail.setFlags(AArch64::TailBranchFlag);
    return MCWinCOFFStreamer::emitInstruction(Tail, STI);
  }
  MCWinCOFFStreamer::emitInstruction(Inst, STI);
}

void AArch64WinCOFFStreamer::emitValueImpl(const MCExpr *Value, unsigned Size,
                                           SMLoc Loc) {
  // The entry of a compressed jump table is a distance in instructions, in
  // one or two bytes. On Windows Itanium and NT-POSIX one that the assembler
  // cannot fold, as across an instruction the linker may resize, is a
  // relocation that states the distance and the shift. The distance is not
  // negative, so either shift states it; the assembler parses the arithmetic
  // one.
  auto *Shift = dyn_cast<MCBinaryExpr>(Value);
  auto *By = Shift ? dyn_cast<MCConstantExpr>(Shift->getRHS()) : nullptr;
  int64_t Folded;
  if (MCII && Size <= 2 && By && By->getValue() == 2 &&
      (Shift->getOpcode() == MCBinaryExpr::LShr ||
       Shift->getOpcode() == MCBinaryExpr::AShr) &&
      !Value->evaluateAsAbsolute(Folded, getAssemblerPtr())) {
    visitUsedExpr(*Value);
    ensureHeadroom(Size);
    addFixup(Shift->getLHS(),
             getCOFFRelocFixupKind(Size == 1 ? COFF::IMAGE_REL_LLVM_USPAN8_W
                                             : COFF::IMAGE_REL_LLVM_USPAN16_W));
    return appendContents(Size, 0);
  }
  MCWinCOFFStreamer::emitValueImpl(Value, Size, Loc);
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

void AArch64TargetWinCOFFStreamer::emitARM64WinCFIEndC() {
  auto &S = getStreamer();
  WinEH::FrameInfo *CurFrame = S.EnsureValidWinFrameInfo(SMLoc());
  if (!CurFrame)
    return;
  bool InEpilogue = S.isInEpilogCFI();
  auto &Instructions = InEpilogue ? S.getCurrentWinEpilog()->Instructions
                                  : CurFrame->Instructions;
  if ((!InEpilogue && CurFrame->PrologEnd) ||
      llvm::any_of(Instructions, [](const WinEH::Instruction &I) {
        return I.Operation == Win64EH::UOP_EndC;
      })) {
    S.getContext().reportError(
        SMLoc(), ".seh_end_c must appear once in a prologue or epilogue");
    return;
  }
  emitARM64WinUnwindCode(Win64EH::UOP_EndC, -1, 0);
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
