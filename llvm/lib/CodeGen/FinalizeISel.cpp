//===-- llvm/CodeGen/FinalizeISel.cpp ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// This pass expands Pseudo-instructions produced by ISel, fixes register
/// reservations and may do machine frame information adjustments.
/// The pseudo instructions are used to allow the expansion to contain control
/// flow, such as a conditional move implemented with a conditional branch and a
/// phi, or an atomic operation implemented with a loop.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/FinalizeISel.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCContext.h"
using namespace llvm;

#define DEBUG_TYPE "finalize-isel"

namespace {
  class FinalizeISel : public MachineFunctionPass {
  public:
    static char ID; // Pass identification, replacement for typeid
    FinalizeISel() : MachineFunctionPass(ID) {}

  private:
    bool runOnMachineFunction(MachineFunction &MF) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      MachineFunctionPass::getAnalysisUsage(AU);
    }
  };
} // end anonymous namespace

/// Where a function's exception tables cover every instruction of an EH
/// scope (usesAsyncEHTables), a block in a scope follows an invoke of
/// llvm.seh.scope.begin that unwinds to the scope's EH pad. Makes each run of
/// such a block's instructions a range of the pad, as an invoke makes its call
/// one. Selection is over, so no run is split, and a later pass adds only
/// what raises nothing after a run: copies and terminators.
static bool addAsyncEHRanges(MachineFunction &MF) {
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  // An invoke's own range stays as it is.
  SmallPtrSet<const MCSymbol *, 16> InvokeBegins, InvokeEnds;
  for (const LandingPadInfo &LP : MF.getLandingPads()) {
    InvokeBegins.insert_range(LP.BeginLabels);
    InvokeEnds.insert_range(LP.EndLabels);
  }

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    const BasicBlock *BB = MBB.getBasicBlock();
    const BasicBlock *Pred = BB ? BB->getSinglePredecessor() : nullptr;
    const auto *II =
        Pred ? dyn_cast<InvokeInst>(Pred->getTerminator()) : nullptr;
    if (!II || II->getIntrinsicID() != Intrinsic::seh_scope_begin ||
        II->getNormalDest() != BB)
      continue;
    unsigned Scope = MF.getEHScopeNumber(II->getUnwindDest());
    if (!Scope)
      continue;
    MachineBasicBlock::iterator Begin = MBB.end();
    auto EndRun = [&](MachineBasicBlock::iterator End) {
      if (Begin == MBB.end())
        return;
      MCSymbol *BeginLabel = MF.getContext().createTempSymbol();
      MCSymbol *EndLabel = MF.getContext().createTempSymbol();
      BuildMI(MBB, Begin, DebugLoc(), TII.get(TargetOpcode::EH_LABEL))
          .addSym(BeginLabel);
      BuildMI(MBB, End, DebugLoc(), TII.get(TargetOpcode::EH_LABEL))
          .addSym(EndLabel);
      MF.addEHScopeInvoke(Scope, BeginLabel, EndLabel);
      Begin = MBB.end();
      Changed = true;
    };
    bool InInvoke = false;
    for (auto I = MBB.getFirstNonPHI(), E = MBB.getFirstTerminator(); I != E;
         ++I) {
      if (I->isEHLabel()) {
        EndRun(I);
        const MCSymbol *Label = I->getOperand(0).getMCSymbol();
        InInvoke = (InInvoke || InvokeBegins.contains(Label)) &&
                   !InvokeEnds.contains(Label);
      } else if (Begin == MBB.end() && !InInvoke && !I->isMetaInstruction()) {
        Begin = I;
      }
    }
    EndRun(MBB.getFirstTerminator());
  }
  return Changed;
}

static std::pair<bool, bool> runImpl(MachineFunction &MF) {
  bool Changed = false;
  bool PreserveCFG = true;
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  const TargetLowering *TLI = MF.getSubtarget().getTargetLowering();

  // GlobalISel finalizes lowering in InstructionSelect.
  if (!MF.getProperties().hasSelected())
    TLI->finalizeLowering(MF);

  // Iterate through each instruction in the function, looking for pseudos.
  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I) {
    MachineBasicBlock *MBB = &*I;
    for (MachineBasicBlock::iterator MBBI = MBB->begin(), MBBE = MBB->end();
         MBBI != MBBE; ) {
      MachineInstr &MI = *MBBI++;

      // Set AdjustsStack to true if the instruction selector emits a stack
      // frame setup instruction or a stack aligning inlineasm.
      if (TII->isFrameInstr(MI) || MI.isStackAligningInlineAsm())
        MF.getFrameInfo().setAdjustsStack(true);

      // If MI is a pseudo, expand it.
      if (MI.usesCustomInsertionHook()) {
        Changed = true;
        MachineBasicBlock *NewMBB = TLI->EmitInstrWithCustomInserter(MI, MBB);
        // The expansion may involve new basic blocks.
        if (NewMBB != MBB) {
          PreserveCFG = false;
          MBB = NewMBB;
          I = NewMBB->getIterator();
          MBBI = NewMBB->begin();
          MBBE = NewMBB->end();
        }
      }
    }
  }

  if (usesAsyncEHTables(MF.getFunction()))
    Changed |= addAsyncEHRanges(MF);
  return {Changed, PreserveCFG};
}

char FinalizeISel::ID = 0;
char &llvm::FinalizeISelID = FinalizeISel::ID;
INITIALIZE_PASS(FinalizeISel, DEBUG_TYPE,
                "Finalize ISel and expand pseudo-instructions", false, false)

bool FinalizeISel::runOnMachineFunction(MachineFunction &MF) {
  return runImpl(MF).first;
}

PreservedAnalyses FinalizeISelPass::run(MachineFunction &MF,
                                        MachineFunctionAnalysisManager &) {
  auto [Changed, PreserveCFG] = runImpl(MF);
  if (!Changed)
    return PreservedAnalyses::all();
  auto PA = getMachineFunctionPassPreservedAnalyses();
  if (PreserveCFG)
    PA.preserveSet<CFGAnalyses>();
  return PA;
}
