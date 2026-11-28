//===- X86CalledCleanups.cpp - Frame access of called cleanup funclets ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Where a function's cleanups are funclets that Windows Itanium's unwinder
// calls (usesCalledCleanups), a funclet is passed the function's establisher
// frame in RDX: the stack pointer that the function's prologue leaves, which is
// where Windows' unwinder finds the frame from any call in the function's body.
// Every stack object is at a fixed offset from it unless the stack is
// realigned, so a function that needs no frame pointer of its own keeps none
// for its funclets. This pass has each funclet take the establisher frame as a
// virtual register and address the frame from it, as the index register of
// every frame reference, which frame index elimination makes the base. A value
// of the function's that the funclet reads it copies where it begins, while
// RDX still holds the establisher frame: the value reaches it in the register
// where the function keeps it, or a reload from the function's frame based on
// RDX, and nothing else of the funclet's touches the function's slots. A value
// that one instruction computes from nothing of the function's is computed
// again there. In every function with called cleanups it records the outgoing
// argument area of each funclet's calls, which the funclet's prologue alone
// allocates.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrBuilder.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/EHPersonalities.h"

using namespace llvm;

#define DEBUG_TYPE "x86-called-cleanups"

namespace {
class X86CalledCleanupsLegacy : public MachineFunctionPass {
public:
  static char ID;
  X86CalledCleanupsLegacy() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  StringRef getPassName() const override {
    return "X86 called cleanup frame access";
  }
};
} // end anonymous namespace

char X86CalledCleanupsLegacy::ID = 0;

INITIALIZE_PASS(X86CalledCleanupsLegacy, DEBUG_TYPE,
                "X86 called cleanup frame access", false, false)

static bool addressFrameFromEstablisher(MachineFunction &MF) {
  if (!MF.hasEHFunclets() || !usesCalledCleanups(MF.getFunction()))
    return false;
  const X86Subtarget &STI = MF.getSubtarget<X86Subtarget>();
  const X86InstrInfo &TII = *STI.getInstrInfo();
  X86MachineFunctionInfo &X86FI = *MF.getInfo<X86MachineFunctionInfo>();
  SmallVector<int, 0> Funclets = getEHFunclets(MF);
  auto FuncletOf = [&](const MachineBasicBlock &MBB) {
    return Funclets[MBB.getNumber()];
  };

  // Each funclet's prologue allocates the outgoing arguments of its calls,
  // which frame layout no longer sees.
  for (const MachineBasicBlock &MBB : MF) {
    if (FuncletOf(MBB) < 0)
      continue;
    unsigned &CallFrame =
        X86FI.getCalledCleanupCallFrames()[MF.getBlockNumbered(FuncletOf(MBB))];
    for (const MachineInstr &MI : MBB)
      if (MI.getOpcode() == TII.getCallFrameSetupOpcode())
        CallFrame = std::max<unsigned>(CallFrame, TII.getFrameSize(MI));
  }

  if (!STI.getFrameLowering()->addressesFrameFromEstablisher(MF))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  for (MachineBasicBlock &Entry : MF) {
    if (!Entry.isEHFuncletEntry())
      continue;
    int Scope = Entry.getNumber();
    MachineBasicBlock::iterator Top =
        Entry.SkipPHIsLabelsAndDebug(Entry.begin());
    DenseMap<Register, Register> Copies;
    SmallVector<std::pair<Register, const MachineInstr *>, 4> Remats;
    for (MachineBasicBlock &MBB : MF) {
      if (FuncletOf(MBB) != Scope)
        continue;
      for (MachineInstr &MI : MBB)
        for (MachineOperand &MO : MI.uses()) {
          if (!MO.isReg() || !MO.getReg().isVirtual())
            continue;
          MachineInstr *Def = MRI.getVRegDef(MO.getReg());
          if (!Def || FuncletOf(*Def->getParent()) == Scope)
            continue;
          Register &Copy = Copies[MO.getReg()];
          if (!Copy) {
            Copy = MRI.cloneVirtualRegister(MO.getReg());
            if (TII.isTriviallyReMaterializable(*Def))
              Remats.emplace_back(Copy, Def);
            else
              BuildMI(Entry, Top, DebugLoc(), TII.get(TargetOpcode::COPY), Copy)
                  .addReg(MO.getReg());
          }
          MO.setReg(Copy);
        }
    }
    // RDX stays live through the copies, so none of them is given it.
    Entry.addLiveIn(X86::RDX);
    Register Frame = MRI.createVirtualRegister(&X86::GR64_NOSPRegClass);
    BuildMI(Entry, Top, DebugLoc(), TII.get(TargetOpcode::COPY), Frame)
        .addReg(X86::RDX);
    // A value that one instruction computes from nothing of the function's is
    // computed again, not kept for the funclet in a callee-saved register.
    for (auto [Reg, Def] : Remats)
      TII.reMaterialize(Entry, Top, Reg, 0, *Def);

    for (MachineBasicBlock &MBB : MF) {
      if (FuncletOf(MBB) != Scope)
        continue;
      for (MachineInstr &MI : MBB) {
        if (MI.isDebugInstr())
          continue;
        for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
          MachineOperand &Base = MI.getOperand(I);
          if (!Base.isFI())
            continue;
          MachineOperand &Index = MI.getOperand(I + X86::AddrIndexReg);
          const TargetRegisterClass *RC =
              TII.getRegClass(MI.getDesc(), I + X86::AddrIndexReg);
          if (!Index.getReg() && (!RC || MRI.constrainRegClass(Frame, RC))) {
            Index.setReg(Frame);
            continue;
          }
          // The reference has an index register of its own.
          RC = TII.getRegClass(MI.getDesc(), I);
          Register Addr =
              MRI.createVirtualRegister(RC ? RC : &X86::GR64RegClass);
          BuildMI(MBB, MI, MI.getDebugLoc(), TII.get(X86::LEA64r), Addr)
              .addFrameIndex(Base.getIndex())
              .addImm(1)
              .addReg(Frame)
              .addImm(0)
              .addReg(0);
          Base.ChangeToRegister(Addr, false);
        }
      }
    }
  }
  return true;
}

bool X86CalledCleanupsLegacy::runOnMachineFunction(MachineFunction &MF) {
  return addressFrameFromEstablisher(MF);
}

FunctionPass *llvm::createX86CalledCleanupsLegacyPass() {
  return new X86CalledCleanupsLegacy();
}

PreservedAnalyses
X86CalledCleanupsPass::run(MachineFunction &MF,
                           MachineFunctionAnalysisManager &MFAM) {
  return addressFrameFromEstablisher(MF)
             ? getMachineFunctionPassPreservedAnalyses()
                   .preserveSet<CFGAnalyses>()
             : PreservedAnalyses::all();
}
