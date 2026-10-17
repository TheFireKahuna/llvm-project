//===- FaultProbeLowering.cpp - Landings for fault probes -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A fault probe is a load, store or atomic access through the target's
// fault-probe address space (TargetLowering::getFaultProbeAddressSpace). It is
// selected as the ordinary instruction, with its memory operand marked
// MOFaultProbe, and on a fault it completes as though the memory read zero and
// discarded the write.
//
// This pass gives each probe a landing: a block that leaves the state the
// access leaves on zero memory (TargetInstrInfo::insertFaultProbeResult) and
// branches to the instruction after the access. The access is recorded as a
// call site of the function's exception table spanning that one instruction,
// from an EH label before it to its post-instruction symbol, with the landing
// as its landing pad; the pad's one catch clause names the function itself,
// which tells a probe's site from an invoke's. A runtime that takes the fault
// resumes at the landing by editing the program counter, and under EH
// continuation guard the landing is a continuation target.
//
// The pass runs after the target's pre-emit passes, so no later pass rewrites
// the access and loses its symbol, and before funclet layout, which files each
// landing with the scope of its access. The landing writes the very registers
// the access defines, so the access's own path is the instruction alone.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/BranchProbability.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "fault-probe-lowering"

namespace {

class FaultProbeLowering : public MachineFunctionPass {
public:
  static char ID;

  FaultProbeLowering() : MachineFunctionPass(ID) {
    initializeFaultProbeLoweringPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  StringRef getPassName() const override {
    return "Fault probe landing insertion";
  }
};

} // end anonymous namespace

char FaultProbeLowering::ID = 0;
char &llvm::FaultProbeLoweringID = FaultProbeLowering::ID;

INITIALIZE_PASS(FaultProbeLowering, DEBUG_TYPE, "Fault probe landing insertion",
                false, false)

static bool isFaultProbe(const MachineInstr &MI) {
  return MI.mayLoadOrStore() &&
         llvm::any_of(MI.memoperands(), [](const MachineMemOperand *MMO) {
           return MMO->isFaultProbe();
         });
}

/// Whether \p Probe merges memory into a register it also reads, as a load
/// folded into another operation does: its result on zero memory is not a
/// value the landing can write. A read-modify-write's tied result is its old
/// value.
static bool mergesIntoRegister(const MachineInstr &Probe) {
  return !Probe.mayStore() &&
         llvm::any_of(Probe.all_defs(),
                      [](const MachineOperand &MO) { return MO.isTied(); });
}

/// The block holding what follows \p Probe, which ends its own block after
/// this: the rest of its block split off, or a new block taking the
/// successors when the probe was already last.
static MachineBasicBlock *splitAfter(MachineInstr &Probe) {
  MachineBasicBlock *MBB = Probe.getParent();
  MachineBasicBlock *Rest = MBB->splitAt(Probe, /*UpdateLiveIns=*/true);
  if (Rest != MBB)
    return Rest;
  MachineFunction &MF = *MBB->getParent();
  Rest = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
  MF.insert(std::next(MBB->getIterator()), Rest);
  Rest->transferSuccessors(MBB);
  MBB->addSuccessor(Rest);
  LivePhysRegs LiveRegs;
  computeAndAddLiveIns(LiveRegs, *Rest);
  return Rest;
}

bool FaultProbeLowering::runOnMachineFunction(MachineFunction &MF) {
  SmallVector<MachineInstr *, 4> Probes;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (isFaultProbe(MI))
        Probes.push_back(&MI);
  if (Probes.empty())
    return false;

  // The landing is a landing pad of an Itanium-style exception table.
  const Function &F = MF.getFunction();
  if (!F.hasPersonalityFn() ||
      isScopedEHPersonality(classifyEHPersonality(F.getPersonalityFn()))) {
    F.getContext().diagnose(DiagnosticInfoUnsupported(
        F, "a fault probe needs an Itanium-style personality function"));
    return false;
  }

  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const bool EHContGuard =
      F.getParent()->getModuleFlag("ehcontguard") &&
      MF.getTarget().getMCAsmInfo().getExceptionHandlingType() ==
          ExceptionHandling::WinEH;
  const unsigned SelfTypeID = MF.getTypeIDFor(&F);

  for (MachineInstr *Probe : Probes) {
    MachineBasicBlock *MBB = Probe->getParent();
    const DebugLoc &DL = Probe->getDebugLoc();
    MachineBasicBlock *Rest = splitAfter(*Probe);

    MachineBasicBlock *Landing = MF.CreateMachineBasicBlock(MBB->getBasicBlock());
    MF.push_back(Landing);
    Landing->setIsEHPad();
    if (EHContGuard) {
      Landing->setIsEHContTarget(true);
      MF.setHasEHContTarget(true);
    }
    LandingPadInfo &LP = MF.getOrCreateLandingPadInfo(Landing);
    LP.LandingPadLabel = MF.getContext().createTempSymbol();
    LP.TypeIds.push_back(SelfTypeID);
    LP.IsFaultProbe = true;
    BuildMI(*Landing, Landing->end(), DL, TII->get(TargetOpcode::EH_LABEL))
        .addSym(LP.LandingPadLabel);

    LivePhysRegs LiveAfter(*TRI);
    LiveAfter.addLiveIns(*Rest);
    if (mergesIntoRegister(*Probe) ||
        !TII->insertFaultProbeResult(*Landing, Landing->end(), *Probe,
                                     LiveAfter))
      F.getContext().diagnose(DiagnosticInfoUnsupported(
          F, "the target cannot give this fault probe its zero result",
          DL));
    TII->insertUnconditionalBranch(*Landing, Rest, DL);
    Landing->addSuccessor(Rest);
    LivePhysRegs LandingLive;
    computeAndAddLiveIns(LandingLive, *Landing);

    MBB->addSuccessor(Landing, BranchProbability::getZero());
    MBB->normalizeSuccProbs();

    // The call site is the access alone: a label before it, and its
    // post-instruction symbol after it.
    MCSymbol *Begin = MF.getContext().createTempSymbol();
    MCSymbol *End = MF.getContext().createTempSymbol();
    BuildMI(*MBB, Probe->getIterator(), DL, TII->get(TargetOpcode::EH_LABEL))
        .addSym(Begin);
    Probe->setPostInstrSymbol(MF, End);
    MF.addInvoke(Landing, Begin, End);
  }
  return true;
}
