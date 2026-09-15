//===- NTRecoverySpills.cpp - Explicit recovery-edge values ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

static cl::opt<bool> EnableRecoverySpills(
    "experimental-nt-recovery-spills", cl::Hidden, cl::init(false),
    cl::desc("Preserve marked experimental NT recovery edges after inlining"));

namespace {
class NTRecoveryLowering : public FunctionPass {
public:
  static char ID;
  NTRecoveryLowering() : FunctionPass(ID) {}
  StringRef getPassName() const override { return "NT recovery intrinsic lowering"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override { AU.setPreservesCFG(); }
  bool runOnFunction(Function &F) override {
    SmallVector<CallBrInst *> Captures;
    for (auto &BB : F)
      if (auto *Capture = dyn_cast<CallBrInst>(BB.getTerminator()))
        if (Capture->getIntrinsicID() == Intrinsic::experimental_nt_recovery ||
            Capture->getIntrinsicID() == Intrinsic::experimental_nt_recovery_scope)
          Captures.push_back(Capture);
    if (Captures.empty())
      return false;
    const Triple &TT = F.getParent()->getTargetTriple();
    bool X64 = TT.getArch() == Triple::x86_64;
    if (!TT.isOSWindows() || TT.isWindowsArm64EC() ||
        (!X64 && TT.getArch() != Triple::aarch64))
      report_fatal_error("NT recovery requires Windows x86-64 or AArch64");
    StringRef Instructions = X64
        ? "leaq ${1:l}(%rip), %rax\n"
          "movq %rax, 8($0)\nmovq %rsp, 16($0)"
        : "adr x16, ${1:l}\n"
          "str x16, [$0, #8]\nmov x16, sp\nstr x16, [$0, #16]";
    StringRef Constraints = X64
        ? "r,!i,~{rax},~{memory},~{dirflag},~{fpsr},~{flags}"
        : "r,!i,~{x16},~{memory}";
    for (CallBrInst *Capture : Captures) {
      if (Capture->getNumIndirectDests() != 1)
        report_fatal_error("NT recovery requires one recovery successor");
      bool Scope = Capture->getIntrinsicID() == Intrinsic::experimental_nt_recovery_scope;
      std::string Assembly = (Twine(X64 ? "# " : "// ") +
          (Scope ? "NT_RECOVERY_SCOPE\n" : "NT_RECOVERY_BUILTIN\n") + Instructions).str();
      auto *Asm = InlineAsm::get(Capture->getFunctionType(), Assembly,
                                Constraints, true);
      IRBuilder<> Builder(Capture);
      auto *Lowered = Builder.CreateCallBr(Asm, Capture->getDefaultDest(),
          {Capture->getIndirectDest(0)}, {Capture->getArgOperand(0)});
      Lowered->setDebugLoc(Capture->getDebugLoc());
      Capture->eraseFromParent();
    }
    return true;
  }
};
char NTRecoveryLowering::ID = 0;

class NTRecoverySpills : public MachineFunctionPass {
  unsigned ImmutablePCReg;
public:
  static char ID;
  NTRecoverySpills(unsigned ImmutablePCReg)
      : MachineFunctionPass(ID), ImmutablePCReg(ImmutablePCReg) {}
  StringRef getPassName() const override { return "NT recovery-edge spills"; }

  bool runOnMachineFunction(MachineFunction &MF) override {
    auto &MRI = MF.getRegInfo();
    const auto &ST = MF.getSubtarget();
    const auto *TII = ST.getInstrInfo();
    const auto *TRI = ST.getRegisterInfo();
    SmallVector<MachineInstr *> Captures;
    for (auto &BB : MF)
      for (auto &MI : BB)
        if (MI.getOpcode() == TargetOpcode::INLINEASM_BR) {
          StringRef Asm = MI.getOperand(0).getSymbolName();
          if (Asm.contains("NT_RECOVERY_BUILTIN") || Asm.contains("NT_RECOVERY_SCOPE") ||
              (EnableRecoverySpills && Asm.contains("NT_RECOVERY_CAPTURE")))
            Captures.push_back(&MI);
        }
    if (Captures.empty())
      return false;
    if (!MRI.isSSA())
      report_fatal_error("recovery preservation requires machine SSA");

    for (MachineInstr *Capture : Captures) {
      MachineBasicBlock *From = Capture->getParent();
      MachineBasicBlock *Target = nullptr;
      for (auto &MO : Capture->operands()) {
        if (!MO.isMBB())
          continue;
        if (Target)
          report_fatal_error("recovery capture requires one recovery label");
        Target = MO.getMBB();
      }
      if (!Target)
        report_fatal_error("recovery capture lacks a recovery label");

      bool FallsToTarget = From->getFallThrough() == Target;
      bool NormalAlsoTargets = FallsToTarget;
      for (auto &MI : From->terminators())
        if (&MI != Capture && MI.isBranch())
          for (auto &MO : MI.operands())
            NormalAlsoTargets |= MO.isMBB() && MO.getMBB() == Target;

      // Loads precede PHI copies as well as ordinary uses at the destination.
      auto *Reload = MF.CreateMachineBasicBlock(Target->getBasicBlock());
      // Do not redirect an unrelated layout predecessor's fallthrough.
      MF.insert(MF.end(), Reload);
      Reload->setIsInlineAsmBrIndirectTarget();
      Reload->setLabelMustBeEmitted();
      Reload->setMachineBlockAddressTaken();
      StringRef Carrier = Capture->getOperand(0).getSymbolName();
      bool Scope = Carrier.contains("NT_RECOVERY_SCOPE");
      if ((Scope && MF.getFunction().getParent()->getModuleFlag("ehcontguard")) ||
          (Carrier.contains("NT_RECOVERY_BUILTIN") &&
           MF.getFunction().getParent()->getModuleFlag("cfguard")))
      {
        // MBB numbers can change later. A separate anchored symbol remains
        // unique while naming the same address as the published block label.
        auto *Symbol = MF.getContext().createLinkerPrivateSymbol("ntrecovery");
        BuildMI(*Reload, Reload->begin(), Capture->getDebugLoc(),
                TII->get(TargetOpcode::EH_LABEL)).addSym(Symbol);
        if (Scope)
          MF.addEHContTarget(Symbol);
        else
          MF.addLongjmpTarget(Symbol);
      }
      for (auto &MO : Capture->operands())
        if (MO.isMBB())
          MO.setMBB(Reload);
      if (NormalAlsoTargets) {
        From->addSuccessor(Reload, BranchProbability::getZero());
        for (auto &Phi : Target->phis())
          for (unsigned I = 1; I < Phi.getNumOperands(); I += 2)
            if (Phi.getOperand(I + 1).getMBB() == From) {
              MachineOperand Value = Phi.getOperand(I);
              Phi.addOperand(MF, Value);
              Phi.addOperand(MF, MachineOperand::CreateMBB(Reload));
              break;
            }
        if (FallsToTarget)
          TII->insertBranch(*From, Target, nullptr, {}, Capture->getDebugLoc());
      } else {
        From->replaceSuccessor(Target, Reload);
        Target->replacePhiUsesWith(From, Reload);
      }
      Reload->addSuccessor(Target);
      TII->insertBranch(*Reload, Target, nullptr, {}, Capture->getDebugLoc());

      SmallPtrSet<MachineBasicBlock *, 32> Reachable;
      SmallVector<MachineBasicBlock *> Worklist{Reload};
      while (!Worklist.empty()) {
        auto *BB = Worklist.pop_back_val();
        if (!Reachable.insert(BB).second)
          continue;
        for (auto *Successor : BB->successors())
          Worklist.push_back(Successor);
      }

      MachineDominatorTree DT(MF);
      unsigned OriginalRegs = MRI.getNumVirtRegs();
      for (unsigned I = 0; I < OriginalRegs; ++I) {
        Register Reg = Register::index2VirtReg(I);
        if (!MRI.hasOneDef(Reg))
          continue;
        auto *Def = MRI.getVRegDef(Reg);
        if (Def == Capture || !DT.dominates(Def, Capture))
          continue;
        SmallVector<MachineOperand *> Uses;
        bool RecoveryLive = false;
        for (auto &Use : MRI.use_nodbg_operands(Reg)) {
          Uses.push_back(&Use);
          auto *UseMI = Use.getParent();
          auto *UseBB = UseMI->getParent();
          if (UseMI->isPHI())
            UseBB = UseMI->getOperand(Use.getOperandNo() + 1).getMBB();
          RecoveryLive |= Reachable.contains(UseBB);
        }
        if (!RecoveryLive)
          continue;

        const auto *RC = MRI.getRegClass(Reg);
        Register Restored = MRI.createVirtualRegister(RC);
        auto Insert = Reload->getFirstTerminator();
        // Look through whole-register copies introduced for register classes.
        auto *Materialize = Def;
        while (Materialize->isCopy() &&
               !Materialize->getOperand(0).getSubReg() &&
               !Materialize->getOperand(1).getSubReg()) {
          Register Source = Materialize->getOperand(1).getReg();
          if (!Source.isVirtual() || !MRI.hasOneDef(Source))
            break;
          Materialize = MRI.getVRegDef(Source);
        }
        bool FixedInputs = true;
        for (const auto &Use : Materialize->all_uses())
          FixedInputs &= !Use.getReg() || Use.getReg() == ImmutablePCReg ||
                         (Use.getReg().isPhysical() &&
                          TRI->isConstantPhysReg(Use.getReg()));
        if (FixedInputs && TII->isTriviallyReMaterializable(*Materialize)) {
          // Constants and fixed frame addresses need no persistent home.
          Register Source = Materialize->getOperand(0).getReg();
          Register Temporary = MRI.createVirtualRegister(MRI.getRegClass(Source));
          TII->reMaterialize(*Reload, Insert, Temporary, 0, *Materialize);
          BuildMI(*Reload, Insert, Capture->getDebugLoc(),
                  TII->get(TargetOpcode::COPY), Restored).addReg(Temporary);
        } else {
          int Slot = MF.getFrameInfo().CreateStackObject(
              TRI->getSpillSize(*RC), TRI->getSpillAlign(*RC), false);
          TII->storeRegToStackSlot(*From, Capture->getIterator(), Reg, false,
                                  Slot, RC, Register());
          TII->loadRegFromStackSlot(*Reload, Insert, Restored, Slot, RC,
                                   Register());
        }
        MachineSSAUpdater Updater(MF);
        Updater.Initialize(Reg);
        Updater.AddAvailableValue(Def->getParent(), Reg);
        Updater.AddAvailableValue(Reload, Restored);
        for (auto *Use : Uses) {
          if (!Use->getParent()->isPHI() &&
              Use->getParent()->getParent() == Def->getParent())
            continue;
          Updater.RewriteUse(*Use);
        }
        MRI.clearKillFlags(Reg);
        if (Def->isDead(MRI)) {
          MRI.markUsesInDebugValueAsUndef(Reg);
          Def->eraseFromParent();
        }
      }
    }
    return true;
  }
};
char NTRecoverySpills::ID = 0;
} // namespace

FunctionPass *llvm::createNTRecoverySpillsPass(unsigned ImmutablePCReg) {
  return new NTRecoverySpills(ImmutablePCReg);
}

FunctionPass *llvm::createNTRecoveryLoweringPass() {
  return new NTRecoveryLowering();
}
