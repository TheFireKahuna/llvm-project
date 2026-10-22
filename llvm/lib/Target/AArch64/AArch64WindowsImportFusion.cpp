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
// A thread-local variable that may be in another image is reached through
// the import pointer of its record: the address of the image's TLS index, then
// the variable's offset in the image's TLS template. Where the record turns
// out to be in the image, the linker knows both: the load of the address can
// become adrp of the index's page, where each load of the index through it is
// described and takes the index's offset in the page, and the load of the
// offset can become a move of it.
//
// A load of the import pointer of data whose pointer a load of the import
// pointer of other data left in a register on every path to it is described
// with that load: where the linker finds both data in one other image, whose
// layout it knows, the load may become an add of the distance between them to
// that register.
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
STATISTIC(NumCalls, "Number of calls described as fused with a load");
STATISTIC(NumValueOnly, "Number of loads described as read only for values");
STATISTIC(NumAnchored, "Number of loads described with loads that may give "
                       "their pointers");

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

// Whether MO names, with the flags of the half that Fragment is of an adrp
// and ldr pair, the import pointer of a thread-local variable's record,
// <name>$tls, which the back end reaches by name.
static bool isThreadLocalRecordPointer(const MachineOperand &MO,
                                       unsigned Fragment) {
  return MO.isSymbol() && (MO.getTargetFlags() & AArch64II::MO_GOT) &&
         (MO.getTargetFlags() & AArch64II::MO_FRAGMENT) == Fragment &&
         StringRef(MO.getSymbolName()).ends_with("$tls");
}

// Whether GV is data rather than a function or an indirect function.
static bool isData(const GlobalValue *GV) {
  if (const auto *GA = dyn_cast<GlobalAlias>(GV))
    GV = GA->getAliaseeObject();
  return GV && !isa<Function, GlobalIFunc>(GV);
}

// Whether GV is data that never changes and whose every definition holds the
// same value, so that a copy of it in any image reads as the data itself does.
static bool isImmutableData(const GlobalValue *GV) {
  const auto *Var = dyn_cast_or_null<GlobalVariable>(GV->getAliaseeObject());
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

// Whether MI adds Reg to another register, unshifted and with neither of them
// the zero register, and reads Reg nowhere else: a load, store or prefetch at
// the sum of the two, or an add of them, which the linker makes take the
// value in Reg as an immediate.
static bool isFusibleIndex(const MachineInstr &MI, Register Reg,
                           const TargetRegisterInfo *TRI) {
  switch (MI.getOpcode()) {
  case AArch64::LDRBBroX:
  case AArch64::LDRHHroX:
  case AArch64::LDRWroX:
  case AArch64::LDRXroX:
  case AArch64::LDRSBWroX:
  case AArch64::LDRSBXroX:
  case AArch64::LDRSHWroX:
  case AArch64::LDRSHXroX:
  case AArch64::LDRSWroX:
  case AArch64::LDRBroX:
  case AArch64::LDRHroX:
  case AArch64::LDRSroX:
  case AArch64::LDRDroX:
  case AArch64::LDRQroX:
  case AArch64::STRBBroX:
  case AArch64::STRHHroX:
  case AArch64::STRWroX:
  case AArch64::STRXroX:
  case AArch64::STRBroX:
  case AArch64::STRHroX:
  case AArch64::STRSroX:
  case AArch64::STRDroX:
  case AArch64::STRQroX:
  case AArch64::PRFMroX:
  case AArch64::ADDXrs:
    break;
  default:
    return false;
  }
  // The operands after the destination or data are the two registers and
  // the shift or extension, which must be none; an add's second register
  // and an access's offset must not be the zero register.
  if (MI.isBundled() || MI.getNumExplicitOperands() < 4 ||
      !MI.getOperand(1).isReg() || !MI.getOperand(2).isReg() ||
      MI.getOperand(MI.getOpcode() == AArch64::ADDXrs ? 3 : 4).getImm())
    return false;
  Register Rn = MI.getOperand(1).getReg(), Rm = MI.getOperand(2).getReg();
  Register Other = Rn == Reg ? Rm : Rm == Reg ? Rn : Register();
  if (!Other || TRI->regsOverlap(Other, Reg) || Other == AArch64::XZR ||
      (MI.getOpcode() == AArch64::ADDXrs &&
       MI.getOperand(0).getReg() == AArch64::XZR))
    return false;
  for (const auto &[I, MO] : enumerate(MI.operands()))
    if (I != 1 && I != 2 && MO.isReg() && MO.getReg() && MO.readsReg() &&
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

// Whether the value Load gives Reg is still in Reg at MI, on every path from
// the function's entry: each path passes Load and nothing after it modifies
// Reg, a call's register mask included.
static bool reachesUnmodified(MachineInstr &Load, MachineInstr &MI,
                              Register Reg, const TargetRegisterInfo *TRI) {
  auto Modifies = [&](MachineBasicBlock::instr_iterator I,
                      MachineBasicBlock::instr_iterator E) {
    return std::any_of(I, E, [&](const MachineInstr &X) {
      return X.modifiesRegister(Reg, TRI);
    });
  };
  MachineBasicBlock *LoadMBB = Load.getParent();
  MachineBasicBlock *MBB = MI.getParent();
  auto AfterLoad = std::next(Load.getIterator());
  if (MBB == LoadMBB)
    for (auto I = AfterLoad; I != LoadMBB->instr_end(); ++I)
      if (&*I == &MI)
        return !Modifies(AfterLoad, I);
  if (Modifies(MBB->instr_begin(), MI.getIterator()))
    return false;
  SmallVector<MachineBasicBlock *, 8> Work(MBB->predecessors());
  SmallPtrSet<MachineBasicBlock *, 8> Seen;
  while (!Work.empty()) {
    MachineBasicBlock *Pred = Work.pop_back_val();
    if (!Seen.insert(Pred).second)
      continue;
    if (Pred == LoadMBB) {
      if (Modifies(AfterLoad, Pred->instr_end()))
        return false;
      continue;
    }
    // A path that reaches the entry, or comes from code the function does
    // not show, does not pass Load.
    if (Pred->pred_empty() || Pred->isEHPad() ||
        Modifies(Pred->instr_begin(), Pred->instr_end()))
      return false;
    append_range(Work, Pred->predecessors());
  }
  return !MBB->pred_empty() && !MBB->isEHPad();
}

bool AArch64WindowsImportFusion::runOnMachineFunction(MachineFunction &MF) {
  // An outlined function does not say which registers its callers read once
  // it returns.
  if (skipFunction(MF.getFunction()) || MF.isOutlined())
    return false;
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  // The loads of import pointers of data, into the register that holds the
  // pointer's page or another, of functions, and of thread-local variables'
  // records.
  SmallVector<MachineInstr *, 8> Loads, CallLoads, RecordLoads;
  // The data whose import pointers an instruction other than such a load, or
  // an adrp of a pointer's page, names.
  SmallPtrSet<const GlobalValue *, 4> OtherUses;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (MI.getOpcode() == AArch64::LDRXui && !MI.isBundled()) {
        if (const GlobalValue *GV =
                getImportPointer(MI.getOperand(2), AArch64II::MO_PAGEOFF)) {
          (isData(GV) ? Loads : CallLoads).push_back(&MI);
          continue;
        }
        if (isThreadLocalRecordPointer(MI.getOperand(2),
                                       AArch64II::MO_PAGEOFF)) {
          RecordLoads.push_back(&MI);
          continue;
        }
      }
      if (MI.getOpcode() == AArch64::ADRP && !MI.isBundled())
        continue;
      for (const MachineOperand &MO : MI.operands())
        if (MO.isGlobal() && (MO.getTargetFlags() & (AArch64II::MO_DLLIMPORT |
                                                     AArch64II::MO_COFFSTUB)))
          OtherUses.insert(MO.getGlobal());
    }
  }
  if (Loads.empty() && CallLoads.empty() && RecordLoads.empty())
    return false;

  ReachingDefInfo RDI;
  RDI.run(MF);
  AArch64FunctionInfo *FI = MF.getInfo<AArch64FunctionInfo>();
  using UseKind = AArch64FunctionInfo::ImportFusedUseKind;
  bool Changed = false;

  // The instructions that read what Load puts in Reg, each with the register
  // it reads, following the copies of the value from register to register,
  // which the register allocator makes around a call and which copy nothing
  // that is read once the linker leaves out what Load loads; if Load or such a
  // copy alone reaches each in Load's section and each accesses memory at an
  // immediate offset from its register.
  using Reader = std::pair<MachineInstr *, Register>;
  auto GetAccesses = [&](MachineInstr *Load, Register Reg,
                         SmallVectorImpl<Reader> &Accesses,
                         bool Index = false) {
    SmallPtrSet<MachineInstr *, 4> Defs = {Load};
    SmallVector<Reader, 4> Work = {{Load, Reg}}, Copies;
    while (!Work.empty()) {
      auto [Def, R] = Work.pop_back_val();
      SmallPtrSet<MachineInstr *, 4> Uses;
      RDI.getGlobalUses(Def, R, Uses);
      for (MachineInstr *Use : Uses) {
        if (Use->getOpcode() == AArch64::ORRXrs && !Use->isBundled() &&
            Use->getOperand(1).getReg() == AArch64::XZR &&
            Use->getOperand(2).getReg() == R && !Use->getOperand(3).getImm() &&
            Use->getOperand(0).getReg() != R) {
          if (Defs.insert(Use).second) {
            Copies.push_back({Use, R});
            Work.push_back({Use, Use->getOperand(0).getReg()});
          }
        } else if (!is_contained(Accesses, Reader{Use, R})) {
          Accesses.push_back({Use, R});
        }
      }
    }
    auto IsReached = [&](const Reader &U) {
      SmallPtrSet<MachineInstr *, 2> Reaching;
      RDI.getGlobalReachingDefs(U.first, U.second, Reaching);
      return !Reaching.empty() && all_of(Reaching, [&](MachineInstr *D) {
        return Defs.contains(D);
      });
    };
    if (Accesses.empty() || !all_of(Copies, IsReached) ||
        !all_of(Accesses, [&](const Reader &U) {
          return U.first->getParent()->getSectionID() ==
                     Load->getParent()->getSectionID() &&
                 IsReached(U) &&
                 (Index ? isFusibleIndex(*U.first, U.second, TRI)
                        : isFusibleAccess(*U.first, U.second, TRI));
        }))
      return false;
    // The linker may leave out what Load loads, and so what the copies copy.
    undefDebugValues(*Load, Reg, TRI);
    for (auto [Copy, R] : Copies)
      undefDebugValues(*Copy, Copy->getOperand(0).getReg(), TRI);
    return true;
  };
  // A load of a function's import pointer whose register only calls and jumps
  // through it read, each in the load's section and reached by the load
  // alone: where the linker knows the function's address without the
  // pointer, or knows a better pointer, it may change the load, the calls
  // still reading its register.
  for (MachineInstr *Load : CallLoads) {
    Register Reg = Load->getOperand(0).getReg();
    SmallPtrSet<MachineInstr *, 4> Uses;
    RDI.getGlobalUses(Load, Reg, Uses);
    if (Uses.empty() || !all_of(Uses, [&](MachineInstr *Use) {
          return Use->isCall() && Use->getOperand(0).isReg() &&
                 Use->getOperand(0).getReg() == Reg &&
                 none_of(drop_begin(Use->operands()),
                         [&](const MachineOperand &MO) {
                           return MO.isReg() && MO.readsReg() &&
                                  TRI->regsOverlap(MO.getReg(), Reg);
                         }) &&
                 Use->getParent()->getSectionID() ==
                     Load->getParent()->getSectionID() &&
                 RDI.getUniqueReachingMIDef(Use, Reg) == Load;
        }))
      continue;
    for (MachineInstr *Use : Uses)
      FI->addImportFusedUse(Use, Load, Reg, UseKind::Call);
    FI->addImportFusedLoad(Load, true);
    NumCalls += Uses.size();
    Changed = true;
  }

  // A record's loads are described as data's are, where the load's register
  // is its adrp's alone. Of them, the load of the record's first word, the
  // address of the TLS index, is a load of each access through its register,
  // and the load of an offset, the variable's or its guard's, a load of each
  // instruction that adds it to another register.
  for (MachineInstr *Load : RecordLoads) {
    Register Reg = Load->getOperand(0).getReg();
    MachineInstr *Adrp = RDI.getUniqueReachingMIDef(Load, Reg);
    SmallPtrSet<MachineInstr *, 2> AdrpUses;
    if (Load->getOperand(1).getReg() != Reg || !Adrp ||
        Adrp->getOpcode() != AArch64::ADRP ||
        !isThreadLocalRecordPointer(Adrp->getOperand(1), AArch64II::MO_PAGE) ||
        StringRef(Adrp->getOperand(1).getSymbolName()) !=
            Load->getOperand(2).getSymbolName())
      continue;
    RDI.getGlobalUses(Adrp, Reg, AdrpUses);
    SmallVector<Reader, 4> Accesses;
    if (AdrpUses.size() != 1 || !GetAccesses(Load, Reg, Accesses))
      continue;
    for (auto [Use, R] : Accesses) {
      FI->addImportFusedUse(Use, Load, R, UseKind::Access);
      SmallVector<Reader, 4> Readers;
      Register Def = Use->getOperand(0).getReg();
      int64_t Word = Use->getOperand(2).getImm();
      bool Index =
          Use->getOpcode() == AArch64::LDRWui && (Word == 2 || Word == 3);
      // The 64-bit register, which blocks list as live-in.
      if (Index)
        Def = TRI->getMatchingSuperReg(Def, AArch64::sub_32,
                                       &AArch64::GPR64RegClass);
      else if (Use->getOpcode() != AArch64::LDRXui || Word)
        continue;
      if (!GetAccesses(Use, Def, Readers, Index))
        continue;
      for (auto [Reader, ReaderReg] : Readers)
        FI->addImportFusedUse(Reader, Use, ReaderReg,
                              Index ? UseKind::Index : UseKind::Access);
      FI->addImportFusedLoad(Use, true);
      NumAccesses += Readers.size();
    }
    FI->addImportFusedLoad(Load, true);
    NumAccesses += Accesses.size();
    Changed = true;
  }
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

  // The loads of the import pointers of data that the register of a load of
  // another such pointer, the nearest earlier ones first, still holds the
  // pointer of, each with that load.
  for (auto [I, Load] : enumerate(Loads)) {
    const GlobalValue *GV = Load->getOperand(2).getGlobal();
    SmallVector<Register, 4> Regs;
    unsigned Tried = 0;
    for (MachineInstr *Anchor : reverse(ArrayRef(Loads).take_front(I))) {
      Register Reg = Anchor->getOperand(0).getReg();
      if (Regs.size() == 4 || Tried == 16)
        break;
      if (Anchor->getOperand(2).getGlobal() == GV || is_contained(Regs, Reg) ||
          Anchor->getParent()->getSectionID() !=
              Load->getParent()->getSectionID())
        continue;
      ++Tried;
      if (!reachesUnmodified(*Anchor, *Load, Reg, TRI))
        continue;
      Regs.push_back(Reg);
      FI->addImportAnchor(Load, Anchor, Reg);
      ++NumAnchored;
      Changed = true;
    }
  }
  return Changed;
}
