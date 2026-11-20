//===---- KCFI.cpp - Implements Kernel Control-Flow Integrity (KCFI) ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass implements Kernel Control-Flow Integrity (KCFI) indirect call
// check lowering. For each call instruction with a cfi-type attribute, it
// emits an arch-specific check before the call, and bundles the check and
// the call to prevent unintentional modifications.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/KCFI.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "kcfi"
#define KCFI_PASS_NAME "Insert KCFI indirect call checks"

STATISTIC(NumKCFIChecksAdded, "Number of indirect call checks added");

namespace {
class KCFI {
public:
  bool run(MachineFunction &MF);

private:
  /// Machine instruction info used throughout the class.
  const TargetInstrInfo *TII = nullptr;

  /// Target lowering for arch-specific parts.
  const TargetLowering *TLI = nullptr;

  /// Emits a KCFI check before an indirect call.
  /// \returns true if the check was added and false otherwise.
  bool emitCheck(MachineBasicBlock &MBB,
                 MachineBasicBlock::instr_iterator I) const;
};

class MachineKCFILegacy : public MachineFunctionPass {
public:
  static char ID;

  MachineKCFILegacy() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return KCFI_PASS_NAME; }
  bool runOnMachineFunction(MachineFunction &MF) override {
    return KCFI().run(MF);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addPreserved<MachineRegisterClassInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char MachineKCFILegacy::ID = 0;
} // end anonymous namespace

INITIALIZE_PASS(MachineKCFILegacy, DEBUG_TYPE, KCFI_PASS_NAME, false, false)

FunctionPass *llvm::createKCFIPass() { return new MachineKCFILegacy(); }

PreservedAnalyses MachineKCFIPass::run(MachineFunction &MF,
                                       MachineFunctionAnalysisManager &MFAM) {
  if (!KCFI().run(MF))
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

bool KCFI::emitCheck(MachineBasicBlock &MBB,
                     MachineBasicBlock::instr_iterator MBBI) const {
  assert(TII && "Target instruction info was not initialized");
  assert(TLI && "Target lowering was not initialized");

  // If the call instruction is bundled, we can only emit a check safely if
  // it's the first instruction in the bundle.
  if (MBBI->isBundled() && !std::prev(MBBI)->isBundle())
    report_fatal_error("Cannot emit a KCFI check for a bundled call");

  // Windows' unwinder knows an epilogue only up to its terminator, so a tail
  // call's check goes before the epilogue, while the frame is whole.
  MachineBasicBlock::instr_iterator Epilogue = MBBI;
  if (MBBI->isReturn() && !MBBI->isBundled() &&
      MBB.getParent()->usesWindowsUnwindInfo())
    while (Epilogue != MBB.instr_begin() &&
           std::prev(Epilogue)->getFlag(MachineInstr::FrameDestroy))
      --Epilogue;
  MachineInstr *EpilogueEnd = Epilogue == MBBI ? nullptr : &*std::prev(MBBI);

  // Emit a KCFI check for the call instruction at MBBI. The implementation
  // must unfold memory operands if applicable.
  MachineInstr *Check = TLI->EmitKCFICheck(MBB, MBBI, TII);
  if (EpilogueEnd)
    MBB.splice(Epilogue, &MBB, std::next(EpilogueEnd->getIterator()), MBBI);

  // Clear the original call's CFI type.
  assert(MBBI->isCall() && "Unexpected instruction type");
  MBBI->setCFIType(*MBB.getParent(), 0);

  // If not already bundled, bundle the check and the call to prevent
  // further changes.
  if (!MBBI->isBundled())
    finalizeBundle(MBB, Check->getIterator(), std::next(MBBI->getIterator()));

  ++NumKCFIChecksAdded;
  return true;
}

bool KCFI::run(MachineFunction &MF) {
  const Module *M = MF.getFunction().getParent();
  if (!M->getModuleFlag("kcfi"))
    return false;

  const auto &SubTarget = MF.getSubtarget();
  TII = SubTarget.getInstrInfo();
  TLI = SubTarget.getTargetLowering();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    // Use instr_iterator because we don't want to skip bundles.
    for (MachineBasicBlock::instr_iterator MII = MBB.instr_begin(),
                                           MIE = MBB.instr_end();
         MII != MIE; ++MII) {
      if (MII->isCall() && MII->getCFIType())
        Changed |= emitCheck(MBB, MII);
    }
  }

  return Changed;
}

// Returns true if MI is a call of a Control Flow Guard check function.
static bool isCFGuardCheck(const MachineInstr &MI) {
  const MachineFunction &MF = *MI.getMF();
  const uint32_t *Mask =
      MF.getSubtarget().getRegisterInfo()->getCallPreservedMask(
          MF, CallingConv::CFGuard_Check);
  return MI.isCall() && any_of(MI.operands(), [&](const MachineOperand &MO) {
           return MO.isRegMask() && MO.getRegMask() == Mask;
         });
}

// Returns true if MI loads a call target that the CFGuard pass proved.
static bool isMarkedLoad(const MachineInstr &MI) {
  return MI.hasOneMemOperand() && ((*MI.memoperands_begin())->getFlags() &
                                   MachineMemOperand::MOCFGuardProven);
}

// Computes again, in front of its use, each constant that MI is computed from
// since the last call in its block, where its one definition is not on that
// stretch. Returns true if it changed anything. A load ends the search unless
// ThroughLoad, since what it reads is a value the proof takes as it comes.
static bool recomputeConstants(MachineInstr &MI, bool ThroughLoad) {
  MachineBasicBlock &MBB = *MI.getParent();
  MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  const TargetInstrInfo &TII = *MBB.getParent()->getSubtarget().getInstrInfo();
  bool Changed = false;
  for (MachineOperand &MO : MI.all_uses()) {
    Register Reg = MO.getReg();
    MachineInstr *Def = Reg.isVirtual() ? MRI.getUniqueVRegDef(Reg) : nullptr;
    if (!Def || Def->isPHI())
      continue;
    auto I = std::next(MI.getReverseIterator());
    while (I != MBB.instr_rend() && !I->isCall() && &*I != Def)
      ++I;
    if (I != MBB.instr_rend() && &*I == Def) {
      if (ThroughLoad || !Def->mayLoad())
        Changed |= recomputeConstants(*Def, /*ThroughLoad=*/false);
      continue;
    }
    if (!TII.isTriviallyReMaterializable(*Def) ||
        Def->getOperand(0).getSubReg())
      continue;
    MO.setReg(MRI.cloneVirtualRegister(Reg));
    TII.reMaterialize(MBB, MI, MO.getReg(), 0, *Def);
    if (MRI.use_nodbg_empty(Reg)) {
      MRI.markUsesInDebugValueAsUndef(Reg);
      Def->eraseFromParent();
    }
    Changed = true;
  }
  return Changed;
}

// MachineLICM keeps a constant such as a table's address in one register
// across the calls of a loop, where a callee may spill it, and
// findProvenCallTarget then finds no proof of a target loaded through it.
// Before register allocation, computes such a constant again in front of the
// load, where it needs a register only as far as the call, and does the same
// for the guard check that the call may lose.
static bool recomputeProvenAddresses(MachineFunction &MF) {
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    for (auto I = MBB.instr_begin(), E = MBB.instr_end(); I != E; ++I) {
      if (!isMarkedLoad(*I))
        continue;
      Changed |= recomputeConstants(*I, /*ThroughLoad=*/false);
      auto Call =
          std::find_if(I, E, [](MachineInstr &MI) { return MI.isCall(); });
      if (Call != E && isCFGuardCheck(*Call))
        Changed |= recomputeConstants(*Call, /*ThroughLoad=*/true);
    }
  return Changed;
}

namespace {
class CFGuardProvenAddressLegacy : public MachineFunctionPass {
public:
  static char ID;

  CFGuardProvenAddressLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return recomputeProvenAddresses(MF);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

char CFGuardProvenAddressLegacy::ID = 0;
} // end anonymous namespace

INITIALIZE_PASS(CFGuardProvenAddressLegacy, "cfguard-proven-address",
                "Recompute the addresses of proven call targets", false, false)

FunctionPass *llvm::createCFGuardProvenAddressLegacyPass() {
  return new CFGuardProvenAddressLegacy();
}

PreservedAnalyses
CFGuardProvenAddressPass::run(MachineFunction &MF,
                              MachineFunctionAnalysisManager &MFAM) {
  if (!recomputeProvenAddresses(MF))
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

// Returns the marked load that defines Reg where Call reads it, if both are
// in one block and the value stayed in registers: only full copies define the
// register in between. The one call that may lie between them is a Control
// Flow Guard check, which preserves the register and is returned in Check;
// any other callee may spill the value to its own frame.
static MachineInstr *findMarkedLoad(MachineInstr &Call, Register Reg,
                                    MachineInstr *&Check) {
  MachineBasicBlock &MBB = *Call.getParent();
  const MachineFunction &MF = *MBB.getParent();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

  Check = nullptr;
  for (MachineInstr &MI :
       make_range(std::next(Call.getReverseIterator()), MBB.instr_rend())) {
    if (MI.isCall()) {
      if (Check || MI.modifiesRegister(Reg, &TRI) || !isCFGuardCheck(MI))
        return nullptr;
      Check = &MI;
    } else if (!MI.modifiesRegister(Reg, &TRI)) {
      continue;
    } else if (auto Copy = TII.isCopyInstr(MI);
               Copy && Copy->Destination->getReg() == Reg) {
      Reg = Copy->Source->getReg();
    } else if (MI.getNumExplicitDefs() == 1 && MI.getOperand(0).isReg() &&
               MI.getOperand(0).getReg() == Reg && isMarkedLoad(MI)) {
      return &MI;
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

// Returns true if every value the address of Load is computed from is one the
// proof takes as it comes or one no writable memory has held since the proof
// read it. Each register is followed back, through the block and the blocks
// that are its only way in, as far as the last call, to a constant or to what
// the proof takes as it comes: a value loaded from memory other than a stack
// slot, a call's result, a register the function was entered with. A value
// from a stack slot, one that a callee may have spilled or one that entered a
// block with several predecessors may have been written since.
static bool hasProvenAddress(const MachineInstr &Load) {
  const MachineBasicBlock *MBB = Load.getParent();
  const MachineFunction &MF = *MBB->getParent();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

  SmallVector<Register, 4> Regs;
  auto AddUses = [&](const MachineInstr &MI) {
    for (const MachineOperand &MO : MI.all_uses())
      if (MO.getReg() && !MO.isUndef() && !MRI.isReserved(MO.getReg()) &&
          !is_contained(Regs, MO.getReg()))
        Regs.push_back(MO.getReg());
  };
  AddUses(Load);
  auto I = std::next(Load.getReverseIterator());
  for (unsigned Blocks = MF.size();; I = MBB->instr_rbegin()) {
    for (; I != MBB->instr_rend() && !I->isCall() && !Regs.empty(); ++I) {
      unsigned NumRegs = Regs.size();
      for (Register Reg : Regs)
        if (I->modifiesRegister(Reg, &TRI) && !I->definesRegister(Reg, &TRI))
          return false;
      erase_if(Regs,
               [&](Register Reg) { return I->definesRegister(Reg, &TRI); });
      if (Regs.size() == NumRegs)
        continue;
      SmallVector<const MachineMemOperand *, 1> Accesses;
      if (I->isInlineAsm() || TII.hasLoadFromStackSlot(*I, Accesses) ||
          (I->mayLoad() && I->memoperands_empty()))
        return false;
      if (!I->mayLoad())
        AddUses(*I);
    }
    if (Regs.empty() || (I == MBB->instr_rend() && MBB == &MF.front()))
      return true;
    if (I != MBB->instr_rend() || MBB->pred_size() != 1 || MBB->isEHPad() ||
        !--Blocks)
      break;
    MBB = *MBB->pred_begin();
  }

  if (I != MBB->instr_rend())
    erase_if(Regs, [&](Register Reg) { return I->definesRegister(Reg, &TRI); });
  return Regs.empty();
}

MachineInstr *llvm::findProvenCallTarget(MachineInstr &Call, Register Reg,
                                         MachineInstr *&Check) {
  MachineInstr *Load = findMarkedLoad(Call, Reg, Check);
  return Load && hasProvenAddress(*Load) ? Load : nullptr;
}

// Erases MI and then each instruction of its block whose only effect was to
// define a register that MI read.
static void eraseWithDeadInputs(MachineInstr &MI) {
  MachineBasicBlock &MBB = *MI.getParent();
  const MachineRegisterInfo &MRI = MBB.getParent()->getRegInfo();
  const TargetRegisterInfo &TRI = *MRI.getTargetRegisterInfo();
  SmallVector<Register, 4> Regs;
  for (const MachineOperand &MO : MI.all_uses())
    if (MO.getReg().isPhysical() && !MRI.isReserved(MO.getReg()))
      Regs.push_back(MO.getReg());
  MachineBasicBlock::iterator End = std::next(MachineBasicBlock::iterator(MI));
  MI.eraseFromParent();
  if (!MRI.tracksLiveness())
    return;
  for (Register Reg : Regs)
    for (MachineInstr &Def : reverse(make_range(MBB.begin(), End)))
      if (Def.modifiesRegister(Reg, &TRI)) {
        if (Def.wouldBeTriviallyDead() &&
            all_of(
                Def.all_defs(),
                [&](const MachineOperand &MO) { return MO.getReg() == Reg; }) &&
            MBB.computeRegisterLiveness(
                &TRI, Reg, std::next(MachineBasicBlock::iterator(Def)),
                MBB.size()) == MachineBasicBlock::LQR_Dead)
          eraseWithDeadInputs(Def);
        break;
      }
}

MachineInstr *llvm::dropProvenCallCheck(MachineInstr &Call, Register Reg,
                                        Register CheckReg) {
  MachineInstr *Check, *None;
  MachineInstr *Load = findMarkedLoad(Call, Reg, Check);
  if (!Load || !Check || findMarkedLoad(*Check, CheckReg, None) != Load ||
      !hasProvenAddress(*Load))
    return nullptr;
  eraseWithDeadInputs(*Check);
  Call.setCFIType(*Call.getMF(), 0);
  return Load;
}
