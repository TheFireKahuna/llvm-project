//===- FaultProbeLowering.cpp - Landings for fault probes -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A fault probe is a 32-bit load through the target's fault-probe address
// space (TargetLowering::getFaultProbeAddressSpace): where it faults, it reads
// zero and execution continues after it. It is selected as the ordinary load,
// never folded into another instruction.
//
// This pass gives each probe a landing: a block that zeroes the load's
// register and branches to the instruction after the load. The load is a call
// site of the function's exception table that spans it alone, with the
// landing as its landing pad, whose one catch clause names the function
// itself, which tells a probe's site from an invoke's. A runtime that takes
// the fault resumes at the landing by setting the program counter.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"

using namespace llvm;

#define DEBUG_TYPE "fault-probe-lowering"

namespace {
class FaultProbeLowering : public MachineFunctionPass {
public:
  static char ID;
  FaultProbeLowering() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
};
} // namespace

char FaultProbeLowering::ID = 0;
char &llvm::FaultProbeLoweringID = FaultProbeLowering::ID;
INITIALIZE_PASS(FaultProbeLowering, DEBUG_TYPE, "Fault probe landings", false,
                false)

bool FaultProbeLowering::runOnMachineFunction(MachineFunction &MF) {
  SmallVector<MachineInstr *, 2> Probes;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (llvm::any_of(MI.memoperands(), [](const MachineMemOperand *MMO) {
            return MMO->isFaultProbe();
          }))
        Probes.push_back(&MI);
  if (Probes.empty())
    return false;

  const Function &F = MF.getFunction();
  auto Unsupported = [&](const char *Why, const DebugLoc &DL) {
    F.getContext().diagnose(DiagnosticInfoUnsupported(F, Why, DL));
  };
  if (!F.hasPersonalityFn() ||
      isFuncletEHPersonality(classifyEHPersonality(F.getPersonalityFn()))) {
    Unsupported("a fault probe needs an Itanium-style personality function",
                Probes.front()->getDebugLoc());
    return false;
  }

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  unsigned Self = MF.getTypeIDFor(&F);
  for (MachineInstr *Probe : Probes) {
    DebugLoc DL = Probe->getDebugLoc();
    if (Probe->mayStore() || Probe->getNumExplicitDefs() != 1 ||
        Probe->getOperand(0).isTied() ||
        !TRI.isGeneralPurposeRegister(MF, Probe->getOperand(0).getReg()) ||
        !Probe->hasOneMemOperand() ||
        (*Probe->memoperands_begin())->getSize() != 4) {
      Unsupported("only a 32-bit load can be a fault probe", DL);
      continue;
    }

    // What follows the probe, in a block of its own.
    MachineBasicBlock *MBB = Probe->getParent();
    MachineBasicBlock *Rest = MBB->splitAt(*Probe);
    if (Rest == MBB) {
      Rest = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
      MF.insert(std::next(MBB->getIterator()), Rest);
      Rest->transferSuccessors(MBB);
      MBB->addSuccessor(Rest);
      LivePhysRegs LiveRegs;
      computeAndAddLiveIns(LiveRegs, *Rest);
    }

    // The landing: the probe's register zero, then what follows.
    MachineBasicBlock *Landing =
        MF.CreateMachineBasicBlock(MBB->getBasicBlock());
    MF.push_back(Landing);
    Landing->setIsEHPad();
    LandingPadInfo &LP = MF.getOrCreateLandingPadInfo(Landing);
    LP.LandingPadLabel = MF.getContext().createTempSymbol();
    LP.TypeIds.push_back(Self);
    BuildMI(*Landing, Landing->end(), DL, TII.get(TargetOpcode::EH_LABEL))
        .addSym(LP.LandingPadLabel);
    TII.buildClearRegister(Probe->getOperand(0).getReg(), *Landing,
                           Landing->end(), DL, /*AllowSideEffects=*/false);
    TII.insertUnconditionalBranch(*Landing, Rest, DL);
    Landing->addSuccessor(Rest);
    LivePhysRegs LiveRegs;
    computeAndAddLiveIns(LiveRegs, *Landing);
    MBB->addSuccessor(Landing, BranchProbability::getZero());
    MBB->normalizeSuccProbs();

    // The call site is the probe alone.
    MCSymbol *Begin = MF.getContext().createTempSymbol();
    MCSymbol *End = MF.getContext().createTempSymbol();
    BuildMI(*MBB, Probe->getIterator(), DL, TII.get(TargetOpcode::EH_LABEL))
        .addSym(Begin);
    Probe->setPostInstrSymbol(MF, End);
    MF.addInvoke(Landing, Begin, End);
  }
  return true;
}
