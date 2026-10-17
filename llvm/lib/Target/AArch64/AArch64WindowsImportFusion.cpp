//===-- AArch64WindowsImportFusion.cpp - Data accesses the linker relaxes -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On Windows Itanium and NT-POSIX, data that may be in another image is
// reached through its import pointer: adrp of the pointer's page, then ldr of
// the pointer. Where the data turns out to be in the image, the linker makes
// the pair compute the data's address, adrp of its page and add of its offset
// in the page. Two cheaper forms are open to it, which this pass describes:
//
// - The memory operands of the accesses through the register, each an
//   immediate offset from it, can take the data's offset in its page, the
//   register keeping the page that adrp gives it; the ldr is then dead. Every
//   use of the register must be such an access, since it then holds the page
//   rather than the data's address.
// - An add of a constant to the register, the next and only instruction to
//   read it, can be folded into the add the ldr becomes, which then computes
//   the sum; the add is then dead.
//
// The linker makes either change only where the offsets it computes fit their
// fields, and leaves both instructions as they are otherwise.
//
// A load of the import pointer of data that never changes, whose register
// only reads of memory read, is described as such, with its adrp: the linker
// may make the pair take the address of the image's own copy of the data,
// which holds the same value wherever the data's address leads.
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/GlobalAlias.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-windows-import-fusion"

STATISTIC(NumAccesses, "Number of accesses described as fused with a load");
STATISTIC(NumAdds, "Number of adds described as fused with a load");
STATISTIC(NumValueOnly, "Number of loads described as read only for values");

namespace {
class AArch64WindowsImportFusion : public MachineFunctionPass {
public:
  static char ID;

  AArch64WindowsImportFusion() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "AArch64 Windows import fusion";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char AArch64WindowsImportFusion::ID = 0;

INITIALIZE_PASS(AArch64WindowsImportFusion, DEBUG_TYPE,
                "AArch64 Windows import fusion", false, false)

FunctionPass *llvm::createAArch64WindowsImportFusionPass() {
  return new AArch64WindowsImportFusion();
}

// The global whose import pointer, or the stub standing in for one, MO names
// with the flags of the half that Fragment is of an adrp and ldr pair.
static const GlobalValue *getImportPointer(const MachineOperand &MO,
                                           unsigned Fragment) {
  if (!MO.isGlobal() || MO.getOffset() != 0 ||
      (MO.getTargetFlags() & AArch64II::MO_FRAGMENT) != Fragment ||
      !(MO.getTargetFlags() & (AArch64II::MO_DLLIMPORT | AArch64II::MO_COFFSTUB)))
    return nullptr;
  return MO.getGlobal();
}

// Whether GV is data rather than a function.
static bool isData(const GlobalValue *GV) {
  if (const auto *GA = dyn_cast<GlobalAlias>(GV))
    GV = GA->getAliaseeObject();
  return GV && !isa<Function>(GV);
}

// Whether GV is data that never changes and whose every definition holds the
// same value, so that a copy of it in any image reads as the data itself does.
static bool isImmutableData(const GlobalValue *GV) {
  const auto *Var = dyn_cast<GlobalVariable>(GV);
  return Var && Var->isConstant() && Var->hasDefinitiveInitializer() &&
         (Var->isWeakForLinker() || Var->hasAvailableExternallyLinkage());
}

// The operand that holds the base register of an access through an
// immediate offset whose field the linker can rewrite, or zero for any other
// opcode: a load, store or prefetch at an unsigned offset, or a pair of loads
// or stores at a signed one.
static unsigned getAccessBaseOperand(unsigned Opcode) {
  switch (Opcode) {
  case AArch64::LDRBBui:
  case AArch64::LDRHHui:
  case AArch64::LDRWui:
  case AArch64::LDRXui:
  case AArch64::LDRSBWui:
  case AArch64::LDRSBXui:
  case AArch64::LDRSHWui:
  case AArch64::LDRSHXui:
  case AArch64::LDRSWui:
  case AArch64::LDRBui:
  case AArch64::LDRHui:
  case AArch64::LDRSui:
  case AArch64::LDRDui:
  case AArch64::LDRQui:
  case AArch64::STRBBui:
  case AArch64::STRHHui:
  case AArch64::STRWui:
  case AArch64::STRXui:
  case AArch64::STRBui:
  case AArch64::STRHui:
  case AArch64::STRSui:
  case AArch64::STRDui:
  case AArch64::STRQui:
  case AArch64::PRFMui:
    return 1;
  case AArch64::LDPWi:
  case AArch64::LDPXi:
  case AArch64::LDPSWi:
  case AArch64::LDPSi:
  case AArch64::LDPDi:
  case AArch64::LDPQi:
  case AArch64::STPWi:
  case AArch64::STPXi:
  case AArch64::STPSi:
  case AArch64::STPDi:
  case AArch64::STPQi:
    return 2;
  default:
    return 0;
  }
}

// Whether MI accesses memory at an immediate offset from Reg, its base, and
// reads Reg nowhere else: the accesses whose offset the linker can make the
// data's offset in its page plus their own.
static bool isFusibleAccess(const MachineInstr &MI, Register Reg,
                            const TargetRegisterInfo *TRI) {
  unsigned Base = getAccessBaseOperand(MI.getOpcode());
  if (!Base || MI.isBundled() || MI.getNumExplicitOperands() != Base + 2 ||
      !MI.getOperand(Base).isReg() || MI.getOperand(Base).getReg() != Reg ||
      !MI.getOperand(Base + 1).isImm())
    return false;
  for (const auto &[I, MO] : enumerate(MI.operands()))
    if (I != Base && MO.isReg() && MO.getReg() && MO.readsReg() &&
        TRI->regsOverlap(MO.getReg(), Reg))
      return false;
  return true;
}

// Whether MI only reads memory through Reg, as the base of its address with
// any offset or index register but Reg: a load, neither volatile nor atomic,
// that neither writes an address back to Reg nor reads it elsewhere, so that
// the address in Reg does not escape. The value loaded may replace it.
static bool isValueRead(const MachineInstr &MI, Register Reg,
                        const TargetRegisterInfo *TRI) {
  if (!MI.mayLoad() || MI.mayStore() || MI.isCall() || MI.isBranch() ||
      MI.hasUnmodeledSideEffects() || MI.isBundled() ||
      MI.getNumOperands() < 3 || MI.memoperands_empty() ||
      any_of(MI.memoperands(), [](const MachineMemOperand *MMO) {
        return MMO->isVolatile() || MMO->isAtomic();
      }))
    return false;
  const MachineOperand &Base = AArch64InstrInfo::getLdStBaseOp(MI);
  if (!Base.isReg() || Base.getReg() != Reg)
    return false;
  for (const MachineOperand &MO : MI.operands()) {
    if (&MO == &Base || !MO.isReg() || !MO.getReg() ||
        !TRI->regsOverlap(MO.getReg(), Reg))
      continue;
    // A write-back definition is tied to the base.
    if (MO.readsReg() || (MO.isDef() && MO.isTied()))
      return false;
  }
  return true;
}

// Whether Opcode, a load at an unsigned offset from its base, has a form that
// reads from an offset from its own address instead, which a linker may give
// it where it finds the data near enough.
static bool hasLiteralForm(unsigned Opcode) {
  switch (Opcode) {
  case AArch64::LDRWui:
  case AArch64::LDRXui:
  case AArch64::LDRSWui:
  case AArch64::LDRSui:
  case AArch64::LDRDui:
  case AArch64::LDRQui:
    return true;
  default:
    return false;
  }
}

// Whether MI, which follows Load in its block, adds an immediate to Reg, or
// subtracts one from it, into a register that nothing between the two reads
// or writes: the linker computes the sum at Load.
static bool isFusibleAdd(const MachineInstr &MI, const MachineInstr &Load,
                         Register Reg, const TargetRegisterInfo *TRI) {
  if ((MI.getOpcode() != AArch64::ADDXri &&
       MI.getOpcode() != AArch64::SUBXri) ||
      MI.isBundled() || MI.getOperand(1).getReg() != Reg ||
      !MI.getOperand(2).isImm())
    return false;
  Register Dest = MI.getOperand(0).getReg();
  bool Same = TRI->regsOverlap(Dest, Reg);
  // A use before Load in its block is reached around a loop.
  for (auto I = std::next(MachineBasicBlock::const_iterator(Load)),
            E = Load.getParent()->end();
       I != E; ++I) {
    if (&*I == &MI)
      return true;
    if (!Same && !I->isDebugInstr() &&
        (I->readsRegister(Dest, TRI) || I->modifiesRegister(Dest, TRI)))
      return false;
  }
  return false;
}

// Where the linker makes the accesses take the data's offset in its page, or
// folds the add into Load, Reg holds the page rather than the data's address
// after Load, so a source variable said to be there is not.
static void undefDebugValues(MachineInstr &Load, Register Reg,
                             const TargetRegisterInfo *TRI) {
  if (!Load.getMF()->getFunction().getSubprogram())
    return;
  SmallVector<std::pair<MachineBasicBlock *, MachineBasicBlock::instr_iterator>,
              8>
      Work = {{Load.getParent(), std::next(Load.getIterator())}};
  SmallPtrSet<MachineBasicBlock *, 8> Seen;
  while (!Work.empty()) {
    auto [MBB, I] = Work.pop_back_val();
    bool Modified = false;
    for (MachineInstr &MI : make_range(I, MBB->instr_end())) {
      if (MI.isDebugValue() &&
          any_of(MI.debug_operands(), [&](const MachineOperand &MO) {
            return MO.isReg() && MO.getReg() &&
                   TRI->regsOverlap(MO.getReg(), Reg);
          })) {
        MI.setDebugValueUndef();
        continue;
      }
      if (MI.modifiesRegister(Reg, TRI)) {
        Modified = true;
        break;
      }
    }
    if (Modified)
      continue;
    for (MachineBasicBlock *Succ : MBB->successors())
      if (Seen.insert(Succ).second)
        Work.push_back({Succ, Succ->instr_begin()});
  }
}

bool AArch64WindowsImportFusion::runOnMachineFunction(MachineFunction &MF) {
  // An outlined function does not say which registers its callers read once
  // it returns.
  if (skipFunction(MF.getFunction()) || MF.isOutlined())
    return false;
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  // The loads of import pointers of data, into the register that holds the
  // pointer's page or another.
  SmallVector<MachineInstr *, 8> Loads;
  // The data whose import pointers an instruction other than such a load, or
  // an adrp of a pointer's page, names.
  SmallPtrSet<const GlobalValue *, 4> OtherUses;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (MI.getOpcode() == AArch64::LDRXui && !MI.isBundled())
        if (const GlobalValue *GV =
                getImportPointer(MI.getOperand(2), AArch64II::MO_PAGEOFF))
          if (isData(GV)) {
            Loads.push_back(&MI);
            continue;
          }
      if (MI.getOpcode() == AArch64::ADRP && !MI.isBundled())
        continue;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isGlobal() && (MO.getTargetFlags() & (AArch64II::MO_DLLIMPORT |
                                                     AArch64II::MO_COFFSTUB)))
          OtherUses.insert(MO.getGlobal());
    }
  }
  if (Loads.empty())
    return false;

  ReachingDefInfo RDI;
  RDI.run(MF);
  AArch64FunctionInfo *FI = MF.getInfo<AArch64FunctionInfo>();
  using UseKind = AArch64FunctionInfo::ImportFusedUseKind;
  bool Changed = false;
  // The loads of import pointers of data that never changes, each with the
  // adrp in its section whose every use is a load of the pointer, which
  // several share where the adrp was hoisted out of a loop; the data some of
  // whose loads have no such adrp, which the linker would not find; and the
  // loads whose registers only reads of memory read.
  SmallPtrSet<MachineInstr *, 8> LoadSet(llvm::from_range, Loads);
  SmallVector<std::pair<MachineInstr *, MachineInstr *>, 4> Paired;
  SmallPtrSet<const GlobalValue *, 4> Unpaired;
  SmallPtrSet<MachineInstr *, 4> ReadOnly;
  DenseMap<MachineInstr *, SmallPtrSet<MachineInstr *, 2>> AdrpLoads;
  for (MachineInstr *Load : Loads) {
    Register Reg = Load->getOperand(0).getReg();
    Register Base = Load->getOperand(1).getReg();
    const GlobalValue *GV = Load->getOperand(2).getGlobal();
    MachineInstr *Adrp = RDI.getUniqueReachingMIDef(Load, Base);
    SmallPtrSet<MachineInstr *, 2> &AdrpUses = AdrpLoads[Adrp];
    if (AdrpUses.empty() && Adrp && Adrp->getOpcode() == AArch64::ADRP &&
        getImportPointer(Adrp->getOperand(1), AArch64II::MO_PAGE) == GV)
      RDI.getGlobalUses(Adrp, Base, AdrpUses);
    if (AdrpUses.empty() || !all_of(AdrpUses, [&](MachineInstr *Use) {
          return LoadSet.contains(Use) &&
                 Use->getOperand(1).getReg() == Base &&
                 Use->getOperand(2).getGlobal() == GV &&
                 RDI.getUniqueReachingMIDef(Use, Base) == Adrp;
        })) {
      Unpaired.insert(GV);
      continue;
    }
    bool OwnAdrp = AdrpUses.size() == 1;
    if (isImmutableData(GV)) {
      if (Adrp->getParent()->getSectionID() ==
          Load->getParent()->getSectionID())
        Paired.push_back({Load, Adrp});
      else
        Unpaired.insert(GV);
    }

    SmallPtrSet<MachineInstr *, 4> Uses;
    RDI.getGlobalUses(Load, Reg, Uses);
    // Where only reads of memory read the address of data that never
    // changes, the address may be that of the image's own copy. The linker
    // finds the adrp in the load's section.
    if (!Uses.empty() && !Paired.empty() && Paired.back().first == Load &&
        all_of(Uses, [&](MachineInstr *Use) {
          return isValueRead(*Use, Reg, TRI);
        }))
      ReadOnly.insert(Load);
    // The accesses take the data's offset in its page, or the add folds into
    // the load, only where the load's register is the adrp's alone.
    if (!OwnAdrp || Reg != Base || Uses.empty() ||
        any_of(Uses, [&](MachineInstr *Use) {
          return Use->getParent()->getSectionID() !=
                     Load->getParent()->getSectionID() ||
                 RDI.getUniqueReachingMIDef(Use, Reg) != Load;
        }))
      continue;
    MachineInstr *Only = *Uses.begin();
    if (Uses.size() == 1 && Only->getParent() == Load->getParent() &&
        isFusibleAdd(*Only, *Load, Reg, TRI)) {
      FI->addImportFusedUse(Only, Load, Reg, UseKind::Add);
      FI->addImportFusedLoad(Load, false);
      undefDebugValues(*Load, Reg, TRI);
      ++NumAdds;
      Changed = true;
      continue;
    }
    if (!all_of(Uses,
                [&](MachineInstr *Use) { return isFusibleAccess(*Use, Reg, TRI); }))
      continue;
    for (MachineInstr *Use : Uses)
      FI->addImportFusedUse(Use, Load, Reg, UseKind::Access);
    FI->addImportFusedLoad(Load, true);
    undefDebugValues(*Load, Reg, TRI);
    NumAccesses += Uses.size();
    Changed = true;
  }

  // Where only reads of memory read the address in the register of every
  // load that an adrp gives the page, the loads may take the address of the
  // image's own copy, all of them together, since the linker makes the adrp
  // give the copy's page. Where the function only reads data that never
  // changes through its address, or compares addresses within it, every load
  // of the data's import pointer may, so that addresses that different loads
  // lead to still compare as before.
  DenseMap<const GlobalValue *, bool> ReadOnlyThrough;
  for (auto [Load, Adrp] : Paired) {
    const GlobalValue *GV = Load->getOperand(2).getGlobal();
    auto [It, Inserted] = ReadOnlyThrough.try_emplace(GV, false);
    if (Inserted)
      It->second =
          !Unpaired.contains(GV) && !OtherUses.contains(GV) &&
          (Load->getOperand(2).getTargetFlags() & AArch64II::MO_DLLIMPORT) &&
          !globalAddressMayBeCapturedIn(GV, MF.getFunction());
    bool AllReadOnly = all_of(AdrpLoads[Adrp], [&](MachineInstr *L) {
      return ReadOnly.contains(L);
    });
    if ((It->second || AllReadOnly) && FI->addImportValueOnlyLoad(Load, Adrp)) {
      ++NumValueOnly;
      Changed = true;
    }
  }

  // Where a load of such data stays for what else reads its register, each
  // use that the load alone reaches and that only reads, in a form that a
  // load from an offset from its own address can take, may read the copy
  // directly. The load is described, if it is not already, as one that other
  // instructions read too.
  for (MachineInstr *Load : Loads) {
    const MachineOperand &Pointer = Load->getOperand(2);
    if (FI->getImportValueOnlyLoads().contains(Load) ||
        !isImmutableData(Pointer.getGlobal()) ||
        !(Pointer.getTargetFlags() & AArch64II::MO_DLLIMPORT))
      continue;
    Register Reg = Load->getOperand(0).getReg();
    SmallPtrSet<MachineInstr *, 4> Uses;
    RDI.getGlobalUses(Load, Reg, Uses);
    for (MachineInstr *Use : Uses) {
      if (!hasLiteralForm(Use->getOpcode()) ||
          !isFusibleAccess(*Use, Reg, TRI) || !isValueRead(*Use, Reg, TRI) ||
          Use->getParent()->getSectionID() !=
              Load->getParent()->getSectionID() ||
          RDI.getUniqueReachingMIDef(Use, Reg) != Load)
        continue;
      FI->addImportFusedLoad(Load, false);
      FI->addImportFusedUse(Use, Load, Reg, UseKind::Access);
      FI->addImportValueOnlyLoad(Use, nullptr);
      ++NumValueOnly;
      Changed = true;
    }
  }
  return Changed;
}
