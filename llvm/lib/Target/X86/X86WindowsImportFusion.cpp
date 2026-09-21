//===-- X86WindowsImportFusion.cpp - Direct calls to local imports --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A call to a function in another DLL reads the address from the import
// address table, and the call instruction names the table entry itself.
// Loop-invariant code motion lifts that read out of a loop and leaves the
// calls inside reading a register, which leaves nothing at the call site that
// names the function. This folds the read back into each call, and marks the
// call so that the linker can undo it.
//
// Which of the two is wanted is not known here. A function defined in the
// image being linked wants the folded call, because the linker makes that one
// direct; a function that really is in another DLL wants the register call,
// because reading the table once is cheaper than reading it on every
// iteration. So both are kept: the read stays where it was, the call carries
// the register it left there, and the linker settles it once it knows where
// the function is. Where nothing settles it the call is the instruction every
// other Windows compiler emits.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "x86-windows-import-fusion"

STATISTIC(NumFolded, "Number of indirect calls folded back onto an import");
STATISTIC(NumRemovable, "Number of import pointer reads left worth nothing");
STATISTIC(NumDropped, "Number of import pointer reads a hoist did not pay for");

namespace {

// The call and jump forms that read a register, against the ones that read
// the pointer. A tail call is still a pseudo here and keeps its own pair.
unsigned getFoldedOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::CALL64r:
    return X86::CALL64m;
  case X86::CALL64r_NT:
    return X86::CALL64m_NT;
  case X86::TAILJMPr64:
    return X86::TAILJMPm64;
  case X86::TAILJMPr64_REX:
    return X86::TAILJMPm64_REX;
  default:
    return 0;
  }
}

// Whether MI reads an import pointer, or a stub standing in for one, into a
// register. The whole of the address has to be the entry, since a call can
// only be given an address the load already had.
bool isImportPointerLoad(const MachineInstr &MI) {
  if (MI.getOpcode() != X86::MOV64rm)
    return false;
  const MachineOperand &Base = MI.getOperand(1 + X86::AddrBaseReg);
  const MachineOperand &Index = MI.getOperand(1 + X86::AddrIndexReg);
  const MachineOperand &Disp = MI.getOperand(1 + X86::AddrDisp);
  const MachineOperand &Segment = MI.getOperand(1 + X86::AddrSegmentReg);
  if (!Base.isReg() || Base.getReg() != X86::RIP || Index.getReg() ||
      Segment.getReg() || !Disp.isGlobal() || Disp.getOffset() != 0)
    return false;
  unsigned Flags = Disp.getTargetFlags();
  return Flags == X86II::MO_DLLIMPORT || Flags == X86II::MO_COFFSTUB;
}

class X86WindowsImportFusion : public MachineFunctionPass {
public:
  static char ID;

  X86WindowsImportFusion() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "X86 Windows import fusion"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<ReachingDefInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace

char X86WindowsImportFusion::ID = 0;

INITIALIZE_PASS_BEGIN(X86WindowsImportFusion, DEBUG_TYPE,
                      "X86 Windows import fusion", false, false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefInfoWrapperPass)
INITIALIZE_PASS_END(X86WindowsImportFusion, DEBUG_TYPE,
                    "X86 Windows import fusion", false, false)

FunctionPass *llvm::createX86WindowsImportFusionPass() {
  return new X86WindowsImportFusion();
}

bool X86WindowsImportFusion::runOnMachineFunction(MachineFunction &MF) {
  const X86Subtarget &ST = MF.getSubtarget<X86Subtarget>();
  if (!ST.is64Bit() ||
      !MF.getTarget().getTargetTriple().isWindowsItaniumOrNTPOSIXEnvironment())
    return false;
  if (skipFunction(MF.getFunction()))
    return false;

  ReachingDefInfo &RDI = getAnalysis<ReachingDefInfoWrapperPass>().getRDI();
  const TargetInstrInfo &TII = *ST.getInstrInfo();
  const TargetRegisterInfo *TRI = ST.getRegisterInfo();

  // Collected before anything is changed, because folding the first use
  // leaves the analysis describing code that is no longer there.
  struct Folded {
    MachineInstr *Load;
    bool AllUses;
    SmallVector<MachineInstr *, 4> Calls;
  };
  SmallVector<Folded, 4> Work;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (!isImportPointerLoad(MI))
        continue;
      Register Reg = MI.getOperand(0).getReg();

      SmallPtrSet<MachineInstr *, 4> Uses;
      RDI.getGlobalUses(&MI, Reg, Uses);
      if (Uses.empty())
        continue;

      // Each call on its own, since the read stays where it is: a use that
      // is something else, a copy of the pointer into the register a tail
      // call has to use among them, simply goes on reading the register. A
      // use this load is not the only definition reaching is left alone, or
      // it would call through an entry holding the other value.
      SmallVector<MachineInstr *, 4> Calls;
      for (MachineInstr *Use : Uses)
        if (getFoldedOpcode(Use->getOpcode()) && Use->getNumOperands() &&
            Use->getOperand(0).isReg() && Use->getOperand(0).getReg() == Reg &&
            RDI.getUniqueReachingMIDef(Use, Reg) == &MI)
          Calls.push_back(Use);
      if (!Calls.empty())
        Work.push_back({&MI, Calls.size() == Uses.size(), std::move(Calls)});
    }
  }

  if (Work.empty())
    return false;

  bool Recompute = false;
  for (auto &[Load, AllUses, Calls] : Work) {
    Register Reg = Load->getOperand(0).getReg();
    bool Mark =
        Load->getOperand(1 + X86::AddrDisp).getTargetFlags() ==
        X86II::MO_DLLIMPORT;
    bool Pointless = AllUses && llvm::all_of(Calls, [&](MachineInstr *Call) {
      return Call->getParent() == Load->getParent();
    });
    for (MachineInstr *Call : Calls) {
      MachineInstrBuilder MIB =
          BuildMI(*Call->getParent(), Call, Call->getDebugLoc(),
                  TII.get(getFoldedOpcode(Call->getOpcode())));
      for (unsigned I = 1 + X86::AddrBaseReg, E = 1 + X86::AddrNumOperands;
           I != E; ++I)
        MIB.add(Load->getOperand(I));
      // Everything the call carried but the register it read: the register
      // mask, the stack and the argument registers.
      for (const MachineOperand &MO : drop_begin(Call->operands()))
        MIB.add(MO);
      MIB.cloneMemRefs(*Load);

      // The register the read left in place still holds, said as a use so
      // that the read is not taken for dead, which it is only once the linker
      // has decided it is not. The call itself is the six bytes every other
      // Windows compiler emits, with nothing added to mark it.
      if (Mark && !Pointless) {
        MIB->setAsmPrinterFlag(X86::AC_IMPORT_FUSE_CALL);
        MIB.addUse(Reg, RegState::Implicit);
      }
      Call->eraseFromParent();
      ++NumFolded;
    }

    // A read in the same block as every call it serves is read as often as
    // they are, so holding the pointer in a register saves nothing and the
    // read is simply gone. What is left is the instruction a compiler that
    // never hoisted it would have emitted.
    if (Pointless) {
      for (MachineBasicBlock &MBB : MF)
        for (MachineInstr &MI : MBB)
          if (MI.isDebugValue() && MI.hasDebugOperandForReg(Reg))
            MI.setDebugValueUndef();
      Load->eraseFromParent();
      Recompute = true;
      ++NumDropped;
      continue;
    }

    // Nothing else reads the register, so if the linker makes every one of
    // those calls direct the read is worth nothing and it says so. The
    // address then lives in the instruction stream and in no register, which
    // is what a variable held in that register has to stop claiming.
    if (Mark && AllUses) {
      // Where the read sits decides what the linker can make of it. Its
      // seven bytes become prefixes on whatever follows, so that has to be an
      // instruction rather than the padding that aligns the next block, and
      // block. A read at the end of its block is moved back one, which costs
      // nothing, since the only thing it reads is the instruction pointer.
      // Whether what follows is short enough, and whether it is a transfer of
      // control that seven more bytes would carry across a thirty-two byte
      // boundary, are settled where the addresses are known.
      MachineBasicBlock *MBB = Load->getParent();
      MachineBasicBlock::iterator It(Load);
      if (std::next(It) == MBB->end() && It != MBB->begin()) {
        MachineInstr &Prev = *std::prev(It);
        if (!Prev.isMetaInstruction() && !Prev.readsRegister(Reg, TRI) &&
            !Prev.modifiesRegister(Reg, TRI) &&
            !Prev.getFlag(MachineInstr::FrameSetup) &&
            !Prev.getFlag(MachineInstr::FrameDestroy))
          MBB->splice(Prev.getIterator(), MBB, It);
      }
      Load->setAsmPrinterFlag(X86::AC_IMPORT_FUSE_LOAD);
      for (MachineBasicBlock &MBB : MF)
        for (MachineInstr &MI : MBB)
          if (MI.isDebugValue() && MI.hasDebugOperandForReg(Reg))
            MI.setDebugValueUndef();
      ++NumRemovable;
    }
  }

  if (Recompute) {
    SmallVector<MachineBasicBlock *, 8> Blocks;
    for (MachineBasicBlock &MBB : MF)
      Blocks.push_back(&MBB);
    fullyRecomputeLiveIns(Blocks);
  }
  return true;
}
