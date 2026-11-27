//===-- FuncletLayout.cpp - Contiguously lay out funclets -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements basic block placement transformations which result in
// funclets being contiguous.
//
//===----------------------------------------------------------------------===//
#include "llvm/CodeGen/FuncletLayout.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
using namespace llvm;

#define DEBUG_TYPE "funclet-layout"

static bool runFuncletLayout(MachineFunction &F) {
  // Even though this gets information from getEHScopeMembership(), this pass is
  // only necessary for funclet-based EH personalities, in which these EH scopes
  // are outlined at the end.
  DenseMap<const MachineBasicBlock *, int> FuncletMembership =
      getEHScopeMembership(F);
  // A shrink-wrapped frame under Windows unwind info is laid out contiguously,
  // its unwind range; a leaf block within it moves to the side of it it
  // follows from.
  SmallPtrSet<const MachineBasicBlock *, 16> Frame = getShrinkWrappedFrame(F);
  if (FuncletMembership.empty() && Frame.empty())
    return false;
  // An epilogue ends its block but for a return or a jump: the unwinder takes
  // what follows one for the body's. Code that tail duplication put there is
  // leaf code, which reads nothing the epilogue restores: ahead of a return it
  // goes ahead of the epilogue, and elsewhere into a block outside the frame.
  auto InEpilogue = [](const MachineInstr &MI) {
    return MI.getFlag(MachineInstr::FrameDestroy);
  };
  for (MachineBasicBlock &MBB : F) {
    auto Epilogue = find_if(MBB, InEpilogue);
    auto Leaf = std::find_if_not(Epilogue, MBB.end(), [&](auto &MI) {
      return InEpilogue(MI) || MI.isDebugInstr();
    });
    if (!Frame.count(&MBB) || Leaf == MBB.end() || Leaf->isReturn() ||
        Leaf->isUnconditionalBranch())
      continue;
    if (MBB.isReturnBlock())
      MBB.splice(MBB.getFirstTerminator(), &MBB, Epilogue, Leaf);
    else
      FuncletMembership[MBB.splitAt(*std::prev(Leaf))] =
          FuncletMembership.lookup(&MBB);
  }
  for (MachineBasicBlock &MBB : F) {
    if (!Frame.count(&MBB))
      continue;
    auto Prologue = find_if(MBB, [](const MachineInstr &MI) {
      return MI.getFlag(MachineInstr::FrameSetup);
    });
    if (Prologue != MBB.end() && Prologue != MBB.getFirstNonDebugInstr()) {
      MBB.splitAt(*std::prev(Prologue));
      Frame = getShrinkWrappedFrame(F);
    }
    break;
  }

  DenseMap<const MachineBasicBlock *, int> Side;
  DenseMap<MachineBasicBlock *, MachineBasicBlock *> LayoutSucc;
  int Current = 0, Own = FuncletMembership.lookup(&F.front());
  for (MachineBasicBlock &MBB : F) {
    if (Frame.count(&MBB))
      Current = 2;
    if (FuncletMembership.lookup(&MBB) == Own)
      Side[&MBB] = Frame.count(&MBB) ? 1 : Current;
    LayoutSucc[&MBB] = MBB.getNextNode();
  }
  F.sort([&](MachineBasicBlock &X, MachineBasicBlock &Y) {
    return std::make_pair(FuncletMembership.lookup(&X), Side.lookup(&X)) <
           std::make_pair(FuncletMembership.lookup(&Y), Side.lookup(&Y));
  });
  if (!Frame.empty()) {
    const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();
    for (MachineBasicBlock &MBB : F) {
      MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
      SmallVector<MachineOperand, 4> Cond;
      if (TII.analyzeBranch(MBB, TBB, FBB, Cond))
        continue;
      if (MBB.getNextNode() != LayoutSucc[&MBB])
        MBB.updateTerminator(LayoutSucc[&MBB]);
      // An epilogue ends in a return or a jump within the frame, the form the
      // unwinder knows: one that would fall out of it into a return ends in
      // its own copy of the return, and into other code jumps there.
      MachineBasicBlock *Next = MBB.getNextNode();
      if (Frame.count(&MBB) && Next && !Frame.count(Next) && !TBB &&
          MBB.isSuccessor(Next) &&
          MBB.getFirstTerminator() == MBB.end() &&
          any_of(MBB, [](const MachineInstr &MI) {
            return MI.getFlag(MachineInstr::FrameDestroy);
          })) {
        auto First = Next->getFirstNonDebugInstr();
        if (First == Next->end() || !First->isReturn()) {
          TII.insertBranch(MBB, Next, nullptr, {}, DebugLoc());
        } else {
          MBB.push_back(F.CloneMachineInstr(&*First));
          MBB.removeSuccessor(Next);
        }
      }
    }
  }

  // Conservatively assume we changed something.
  return true;
}

namespace {
class FuncletLayoutLegacy : public MachineFunctionPass {
public:
  static char ID; // Pass identification, replacement for typeid
  FuncletLayoutLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &F) override {
    return runFuncletLayout(F);
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
};
} // namespace

char FuncletLayoutLegacy::ID = 0;
char &llvm::FuncletLayoutID = FuncletLayoutLegacy::ID;
INITIALIZE_PASS(FuncletLayoutLegacy, DEBUG_TYPE,
                "Contiguously Lay Out Funclets", false, false)

PreservedAnalyses FuncletLayoutPass::run(MachineFunction &MF,
                                         MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);
  if (!runFuncletLayout(MF))
    return PreservedAnalyses::all();

  return getMachineFunctionPassPreservedAnalyses();
}
