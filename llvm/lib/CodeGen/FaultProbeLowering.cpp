//===- FaultProbeLowering.cpp - Faulting ops for probing accesses ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A probing access is a load or store whose fault is a branch: at the IR
// level a callbr of llvm.fault.probe.*, and after selection a load or store
// whose memory operand carries MOFaultProbe, in a block whose one indirect
// successor is the fault destination. This pass wraps each such access in a
// FAULTING_OP naming that successor as its handler, splitting the block so the
// faulting op ends it, and records the access as a call site of the
// function's exception table whose landing pad is the fault destination: the
// site spans the one instruction, from an EH label before the op to the op's
// post-instruction symbol, and the pad's one catch clause names the function
// itself, which tells a probe's site from an invoke's; the destination stays
// an ordinary block, since normal control flow may reach it as well. A runtime that takes
// the fault finds the destination the way it finds any landing pad, and under
// EH continuation guard the destination is a continuation target like one. The AsmPrinter
// also records the pair in the fault map where the object format has one.
// The pass runs after register allocation, as the implicit null checks do: a
// faulting op is a terminator, so no spill may follow the value it defines.
// On a target that cannot lower FAULTING_OP the access stays plain and the
// fault destination is never taken.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/FaultMaps.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
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
    return "Lower probing accesses to faulting ops";
  }
};

} // end anonymous namespace

char FaultProbeLowering::ID = 0;
char &llvm::FaultProbeLoweringID = FaultProbeLowering::ID;

INITIALIZE_PASS(FaultProbeLowering, DEBUG_TYPE,
                "Lower probing accesses to faulting ops", false, false)

static bool isProbe(const MachineInstr &MI) {
  if (!MI.mayLoadOrStore())
    return false;
  for (const MachineMemOperand *MMO : MI.memoperands())
    if (MMO->isFaultProbe())
      return true;
  return false;
}

/// The block's fault destination: its one indirect successor.
static MachineBasicBlock *handlerOf(const MachineBasicBlock &MBB) {
  MachineBasicBlock *Handler = nullptr;
  for (MachineBasicBlock *Succ : MBB.successors()) {
    if (!Succ->isInlineAsmBrIndirectTarget())
      continue;
    assert(!Handler && "a probe block has one fault destination");
    Handler = Succ;
  }
  return Handler;
}

/// Replace MI with a FAULTING_OP that performs the same access, defining the
/// same register, and branches to Handler if the access faults.
static MachineInstr *wrapInFaultingOp(MachineInstr &MI,
                                      MachineBasicBlock *Handler,
                                      const TargetInstrInfo *TII) {
  unsigned NumDefs = MI.getDesc().getNumDefs();
  assert(NumDefs <= 1 && "a probing access defines at most one register");
  Register DefReg;
  if (NumDefs != 0)
    DefReg = MI.getOperand(0).getReg();

  FaultMaps::FaultKind FK;
  if (MI.mayLoad())
    FK = MI.mayStore() ? FaultMaps::FaultingLoadStore : FaultMaps::FaultingLoad;
  else
    FK = FaultMaps::FaultingStore;

  auto MIB = BuildMI(*MI.getParent(), MI.getIterator(), MI.getDebugLoc(),
                     TII->get(TargetOpcode::FAULTING_OP), DefReg)
                 .addImm(FK)
                 .addMBB(Handler)
                 .addImm(MI.getOpcode());
  for (auto &MO : MI.uses()) {
    if (MO.isReg()) {
      MachineOperand NewMO = MO;
      if (MO.isUse())
        NewMO.setIsKill(false);
      else
        NewMO.setIsDead(false);
      MIB.add(NewMO);
    } else {
      MIB.add(MO);
    }
  }
  MIB.setMemRefs(MI.memoperands());
  MI.eraseFromParent();
  return MIB;
}

/// Record Handler as a landing pad, once: the pad label at its start, one
/// catch clause naming the function itself, which marks the pad as a probe's
/// fault destination rather than an invoke's, and under EH continuation guard
/// the continuation mark the kernel's check needs. The block stays an
/// ordinary one rather than an EH pad: normal control flow may reach it too,
/// and a pad is laid out as if nothing fell through into it.
static void makeLandingPad(MachineFunction &MF, MachineBasicBlock *Handler,
                           const TargetInstrInfo *TII, bool EHContGuard) {
  LandingPadInfo &LP = MF.getOrCreateLandingPadInfo(Handler);
  if (LP.LandingPadLabel)
    return;
  LP.LandingPadLabel = MF.getContext().createTempSymbol();
  LP.TypeIds.push_back(MF.getTypeIDFor(&MF.getFunction()));
  LP.IsFaultProbe = true;
  BuildMI(*Handler, Handler->begin(), DebugLoc(),
          TII->get(TargetOpcode::EH_LABEL))
      .addSym(LP.LandingPadLabel);
  if (EHContGuard) {
    Handler->setIsEHContTarget(true);
    MF.setHasEHContTarget(true);
  }
}

bool FaultProbeLowering::runOnMachineFunction(MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  if (!TII->supportsFaultingOps())
    return false;
  const bool EHContGuard =
      MF.getFunction().getParent()->getModuleFlag("ehcontguard") &&
      MF.getTarget().getMCAsmInfo().getExceptionHandlingType() ==
          ExceptionHandling::WinEH;

  SmallVector<MachineInstr *, 4> Probes;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB)
      if (isProbe(MI))
        Probes.push_back(&MI);
  if (Probes.empty())
    return false;

  for (MachineInstr *MI : Probes) {
    MachineBasicBlock *MBB = MI->getParent();
    MachineBasicBlock *Handler = handlerOf(*MBB);
    if (!Handler)
      continue;
    DebugLoc DL = MI->getDebugLoc();
    // The faulting op ends its block. What followed the access falls through
    // to a new block, which inherits the successors; an access that already
    // ended its block falls through to the block's one other successor.
    MachineBasicBlock *Rest = nullptr;
    if (std::next(MI->getIterator()) == MBB->end()) {
      for (MachineBasicBlock *Succ : MBB->successors())
        if (Succ != Handler) {
          assert(!Rest && "an access ending its block has one fall-through");
          Rest = Succ;
        }
      assert(Rest && "an access ending its block falls through somewhere");
    } else {
      Rest = MBB->splitAt(*MI, /*UpdateLiveIns=*/true);
      MBB->addSuccessor(Handler, BranchProbability::getZero());
      // The fault destination belongs to the block that holds the access; a
      // block with no access left cannot reach it.
      if (llvm::none_of(*Rest, isProbe))
        Rest->removeSuccessor(Handler);
    }
    MachineInstr *Op = wrapInFaultingOp(*MI, Handler, TII);
    // The call site is the faulting op alone: a label before it, and its
    // post-instruction symbol after it, which follows the op even though the
    // op ends the block.
    MCSymbol *Begin = MF.getContext().createTempSymbol();
    MCSymbol *End = MF.getContext().createTempSymbol();
    BuildMI(*MBB, Op->getIterator(), DL, TII->get(TargetOpcode::EH_LABEL))
        .addSym(Begin);
    Op->setPostInstrSymbol(MF, End);
    makeLandingPad(MF, Handler, TII, EHContGuard);
    MF.addInvoke(Handler, Begin, End);
    TII->insertBranch(*MBB, Rest, nullptr, {}, DL);
    MBB->normalizeSuccProbs();
  }
  return true;
}
