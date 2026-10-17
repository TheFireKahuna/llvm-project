//===-- X86WindowsImportFusion.cpp - Calls the linker chooses -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Machine loop-invariant code motion unfolds the memory operand of a call
// through an import pointer inside a loop, hoists the load, and leaves a call
// through a register behind, which is the best call to a function in another
// image. A call to a function in the image is best made direct, and one
// through a delay-loaded import's pointer best reads the pointer, which holds
// the loader's stub until the first call. Which is which is known only at the
// link.
//
// This pass describes each such call to the linker, with the load whose
// register it reads, and the asm printer pads it to the length of the same
// call through the pointer, so that the linker can make it either. A call
// through the pointer that the load's register still holds, such as one that
// a test of the pointer guards, is made a call through the register and
// described the same way. A load that only such calls read is dead where the
// linker makes none of them read the register, and the linker disposes of it;
// one that serves a single call in its own block saves nothing, and is folded
// into the call.
//
// A load of the import pointer of data is described with each instruction
// that reads its register only as the base of a memory operand with no index:
// where the data is in the image, the linker makes each such operand
// RIP-relative, and the load dead where nothing else reads the register. One
// whose register is next given a constant, by an add or an lea, is described
// with that instruction: where the data is in the image, the linker makes the
// load take the sum and the add dead.
//
// A load of the import pointer of data that never changes, whose register
// only reads of memory read, is described as such: the linker may make it
// take the address of the image's own copy of the data, which holds the same
// value wherever the data's address leads.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/Module.h"

using namespace llvm;

#define DEBUG_TYPE "x86-windows-import-fusion"

STATISTIC(NumFused, "Number of calls described as fused with a load");
STATISTIC(NumDeleted, "Number of import pointer loads folded into a call");
STATISTIC(NumAccesses, "Number of accesses described as fused with a load");
STATISTIC(NumAdds, "Number of adds described as fused with a load");
STATISTIC(NumValueOnly, "Number of loads described as read only for values");

namespace {

class X86WindowsImportFusion {
  const TargetInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;

  void foldCall(MachineInstr &Call, const MachineInstr &Load);
  MachineInstr *callThroughRegister(MachineInstr &Call, Register Reg);
  bool reachesUnmodified(MachineInstr &Load, MachineInstr &Call, Register Reg,
                         SmallVectorImpl<MachineBasicBlock *> &Blocks) const;
  void undefDebugValues(MachineInstr &Load, Register Reg) const;
  void moveOffBlockEnd(MachineInstr &Load, Register Reg) const;
  bool isFusibleAccess(const MachineInstr &MI, Register Reg) const;
  bool isFusibleAdd(const MachineInstr &MI, Register Reg) const;
  bool isValueRead(const MachineInstr &MI, Register Reg) const;

public:
  bool run(MachineFunction &MF, function_ref<ReachingDefInfo &()> GetRDI);
};

class X86WindowsImportFusionLegacy : public MachineFunctionPass {
public:
  static char ID;

  X86WindowsImportFusionLegacy() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "X86 Windows import fusion"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace

char X86WindowsImportFusionLegacy::ID = 0;

INITIALIZE_PASS(X86WindowsImportFusionLegacy, DEBUG_TYPE,
                "X86 Windows import fusion", false, false)

FunctionPass *llvm::createX86WindowsImportFusionLegacyPass() {
  return new X86WindowsImportFusionLegacy();
}

// The call or jump through a pointer in memory that does what Opcode does
// through a register, or zero. A jump that ends an epilogue has REX.W, which
// the unwinder requires of one through a register.
static unsigned getFoldedOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::CALL64r:
    return X86::CALL64m;
  case X86::TAILJMPr64_REX:
    return X86::TAILJMPm64_REX;
  default:
    return 0;
  }
}

static unsigned getUnfoldedOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::CALL64m:
    return X86::CALL64r;
  case X86::TAILJMPm64_REX:
    return X86::TAILJMPr64_REX;
  default:
    return 0;
  }
}

// Whether a call or jump with Opcode, through a register, can go through Reg
// in the form the linker rewrites: the unwinder takes a jump to end an
// epilogue through only the first sixteen registers.
static bool canGoThrough(unsigned Opcode, Register Reg,
                         const TargetInstrInfo *TII,
                         const TargetRegisterInfo *TRI) {
  if (Opcode != X86::CALL64r &&
      (Opcode != X86::TAILJMPr64_REX || TRI->getEncodingValue(Reg) >= 16))
    return false;
  const TargetRegisterClass *RC = TII->getRegClass(TII->get(Opcode), 0);
  return RC && RC->contains(Reg);
}

// The import pointer, or the stub standing in for one, that the address
// operands of MI starting at Op are exactly, which the linker can tell from
// the relocation alone.
static const MachineOperand *getImportPointer(const MachineInstr &MI,
                                              unsigned Op) {
  const MachineOperand &Base = MI.getOperand(Op + X86::AddrBaseReg);
  const MachineOperand &Scale = MI.getOperand(Op + X86::AddrScaleAmt);
  const MachineOperand &Index = MI.getOperand(Op + X86::AddrIndexReg);
  const MachineOperand &Disp = MI.getOperand(Op + X86::AddrDisp);
  const MachineOperand &Segment = MI.getOperand(Op + X86::AddrSegmentReg);
  if (!Base.isReg() || Base.getReg() != X86::RIP || !Scale.isImm() ||
      Scale.getImm() != 1 || Index.getReg() || Segment.getReg() ||
      !Disp.isGlobal() || Disp.getOffset() != 0)
    return nullptr;
  unsigned Flags = Disp.getTargetFlags();
  if (Flags != X86II::MO_DLLIMPORT && Flags != X86II::MO_COFFSTUB)
    return nullptr;
  return &Disp;
}

static const MachineOperand *getLoadedPointer(const MachineInstr &MI) {
  if (MI.getOpcode() != X86::MOV64rm)
    return nullptr;
  return getImportPointer(MI, 1);
}

// Whether Pointer is the import pointer of data rather than of a function.
static bool isDataPointer(const MachineOperand &Pointer) {
  const GlobalValue *GV = Pointer.getGlobal();
  if (const auto *GA = dyn_cast<GlobalAlias>(GV))
    GV = GA->getAliaseeObject();
  return GV && !isa<Function>(GV);
}

// Whether Pointer is the import pointer of data that never changes and whose
// every definition holds the same value, so that a copy of it in any image
// reads as the data itself does.
static bool isImmutableData(const MachineOperand &Pointer) {
  const auto *GV = dyn_cast<GlobalVariable>(Pointer.getGlobal());
  return GV && GV->isConstant() && GV->hasDefinitiveInitializer() &&
         (GV->isWeakForLinker() || GV->hasAvailableExternallyLinkage());
}

static bool isSamePointer(const MachineOperand &A, const MachineOperand &B) {
  return A.getGlobal() == B.getGlobal() &&
         A.getTargetFlags() == B.getTargetFlags();
}

static bool isInSameSection(const MachineInstr &A, const MachineInstr &B) {
  return A.getParent()->getSectionID() == B.getParent()->getSectionID();
}

// Replaces Call with New, which carries what Call did after the operands
// that say where it goes: the register mask, the stack and the argument
// registers.
static MachineInstr *replaceCall(MachineInstr &Call, MachineInstr *New,
                                 unsigned FirstOther) {
  MachineFunction &MF = *Call.getMF();
  MachineInstrBuilder MIB(MF, New);
  for (const MachineOperand &MO : drop_begin(Call.operands(), FirstOther))
    MIB.add(MO);
  New->setFlags(Call.getFlags());
  New->cloneInstrSymbols(MF, Call);
  if (Call.shouldUpdateAdditionalCallInfo())
    MF.moveAdditionalCallInfo(&Call, New);
  Call.eraseFromParent();
  return New;
}

// Folds the pointer Load reads into Call, a call through Load's register.
void X86WindowsImportFusion::foldCall(MachineInstr &Call,
                                      const MachineInstr &Load) {
  MachineFunction &MF = *Call.getMF();
  MachineInstr *New = MF.CreateMachineInstr(
      TII->get(getFoldedOpcode(Call.getOpcode())), Call.getDebugLoc(),
      /*NoImplicit=*/true);
  Call.getParent()->insert(Call.getIterator(), New);
  MachineInstrBuilder MIB(MF, New);
  for (unsigned I = 1 + X86::AddrBaseReg, E = 1 + X86::AddrNumOperands; I != E;
       ++I)
    MIB.add(Load.getOperand(I));
  MIB.cloneMemRefs(Load);
  replaceCall(Call, New, 1);
}

// Notes that Call, a call through a register, calls the function whose
// pointer Load reads, which the call itself no longer names.
static void noteCallee(MachineInstr &Call, const MachineInstr &Load) {
  const MachineOperand &Pointer = *getLoadedPointer(Load);
  Call.getMF()->addCalledGlobal(
      &Call, {Pointer.getGlobal(), Pointer.getTargetFlags()});
}

// The call through Reg that replaces Call, a call through the pointer that
// Reg holds.
MachineInstr *X86WindowsImportFusion::callThroughRegister(MachineInstr &Call,
                                                          Register Reg) {
  MachineFunction &MF = *Call.getMF();
  MachineInstr *New = MF.CreateMachineInstr(
      TII->get(getUnfoldedOpcode(Call.getOpcode())), Call.getDebugLoc(),
      /*NoImplicit=*/true);
  Call.getParent()->insert(Call.getIterator(), New);
  MachineInstrBuilder(MF, New).addReg(Reg);
  return replaceCall(Call, New, X86::AddrNumOperands);
}

// Whether the value Load gives Reg is still in Reg at Call, on every path from
// the function's entry: each path passes Load and nothing after it modifies
// Reg, a call's register mask included. Reg need not be live there, so the
// reaching definitions, which follow liveness, cannot tell. Blocks gets the
// blocks after Load's on those paths, in which Reg then becomes live.
bool X86WindowsImportFusion::reachesUnmodified(
    MachineInstr &Load, MachineInstr &Call, Register Reg,
    SmallVectorImpl<MachineBasicBlock *> &Blocks) const {
  auto Modifies = [&](MachineBasicBlock::instr_iterator I,
                      MachineBasicBlock::instr_iterator E) {
    return std::any_of(I, E, [&](const MachineInstr &MI) {
      return MI.modifiesRegister(Reg, TRI);
    });
  };
  MachineBasicBlock *LoadMBB = Load.getParent();
  MachineBasicBlock *CallMBB = Call.getParent();
  auto AfterLoad = std::next(Load.getIterator());
  if (CallMBB == LoadMBB) {
    for (auto I = AfterLoad; I != LoadMBB->instr_end(); ++I)
      if (&*I == &Call)
        return !Modifies(AfterLoad, I);
  }
  if (Modifies(CallMBB->instr_begin(), Call.getIterator()))
    return false;
  SmallVector<MachineBasicBlock *, 8> Work(CallMBB->predecessors());
  SmallPtrSet<MachineBasicBlock *, 8> Seen;
  Blocks.push_back(CallMBB);
  while (!Work.empty()) {
    MachineBasicBlock *MBB = Work.pop_back_val();
    if (!Seen.insert(MBB).second)
      continue;
    if (MBB == LoadMBB) {
      if (Modifies(AfterLoad, MBB->instr_end()))
        return false;
      continue;
    }
    // A path that reaches the entry, or comes from code the function does
    // not show, does not pass Load.
    if (MBB->pred_empty() || MBB->isEHPad() ||
        Modifies(MBB->instr_begin(), MBB->instr_end()))
      return false;
    if (MBB != CallMBB)
      Blocks.push_back(MBB);
    append_range(Work, MBB->predecessors());
  }
  return !CallMBB->pred_empty() && !CallMBB->isEHPad();
}

// The linker may dispose of Load, which leaves Reg holding nothing where the
// value Load gave it reaches, so a source variable said to be there is not.
void X86WindowsImportFusion::undefDebugValues(MachineInstr &Load,
                                              Register Reg) const {
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

// A load the linker disposes of gives its bytes to the instruction that
// follows it, which must then be in its block rather than after the padding
// that may align the next one. Moving the load back over an instruction that
// neither reads nor writes its register costs nothing.
void X86WindowsImportFusion::moveOffBlockEnd(MachineInstr &Load,
                                             Register Reg) const {
  MachineBasicBlock *MBB = Load.getParent();
  if (next_nodbg(Load.getIterator(), MBB->instr_end()) != MBB->instr_end() ||
      Load.getIterator() == MBB->instr_begin())
    return;
  auto Prev = prev_nodbg(Load.getIterator(), MBB->instr_begin());
  if (Prev->isMetaInstruction() || Prev->isPseudo() || Prev->isBundled() ||
      Prev->readsRegister(Reg, TRI) || Prev->modifiesRegister(Reg, TRI) ||
      Prev->getFlag(MachineInstr::FrameSetup) ||
      Prev->getFlag(MachineInstr::FrameDestroy))
    return;
  MBB->splice(Prev, MBB, Load.getIterator());
}

// The register that the base of MI's memory operand is, with no index or
// segment and a displacement that needs no relocation, or none.
static Register getAccessBase(const MachineInstr &MI) {
  const MCInstrDesc &Desc = MI.getDesc();
  int MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
  if (MemOp < 0)
    return Register();
  MemOp += X86II::getOperandBias(Desc);
  const MachineOperand &Base = MI.getOperand(MemOp + X86::AddrBaseReg);
  if (!Base.isReg() || MI.getOperand(MemOp + X86::AddrIndexReg).getReg() ||
      MI.getOperand(MemOp + X86::AddrSegmentReg).getReg() ||
      !MI.getOperand(MemOp + X86::AddrDisp).isImm())
    return Register();
  return Base.getReg();
}

// Whether MI reads or writes memory through Reg in a form the linker can make
// RIP-relative: a legacy, VEX or EVEX encoding outside APX's map 4, whose
// memory operand has Reg as its base, with no index, and that reads Reg
// nowhere else, which the linker leaves unfilled where it makes the operand
// RIP-relative and drops the load.
bool X86WindowsImportFusion::isFusibleAccess(const MachineInstr &MI,
                                             Register Reg) const {
  const MCInstrDesc &Desc = MI.getDesc();
  uint64_t Encoding = Desc.TSFlags & X86II::EncodingMask;
  if ((Encoding != X86II::LEGACY && Encoding != X86II::VEX &&
       Encoding != X86II::EVEX) ||
      (Desc.TSFlags & X86II::OpMapMask) == X86II::T_MAP4 || MI.isBundled() ||
      MI.isPseudo() || getAccessBase(MI) != Reg ||
      TRI->getEncodingValue(Reg) >= 16)
    return false;
  int MemOp =
      X86II::getMemoryOperandNo(Desc.TSFlags) + X86II::getOperandBias(Desc);
  for (const auto &[I, MO] : enumerate(MI.operands())) {
    if (I >= unsigned(MemOp) && I < unsigned(MemOp) + X86::AddrNumOperands)
      continue;
    if (MO.isReg() && MO.getReg() && MO.readsReg() &&
        TRI->regsOverlap(MO.getReg(), Reg))
      return false;
  }
  return true;
}

// Whether MI adds a constant to Reg, writing the sum to a register the linker
// can name: add or sub of an immediate with the flags dead, or lea of Reg plus
// a displacement.
bool X86WindowsImportFusion::isFusibleAdd(const MachineInstr &MI,
                                          Register Reg) const {
  switch (MI.getOpcode()) {
  case X86::ADD64ri8:
  case X86::ADD64ri32:
  case X86::SUB64ri8:
  case X86::SUB64ri32:
    return MI.getOperand(0).getReg() == Reg &&
           MI.getOperand(1).getReg() == Reg && MI.getOperand(2).isImm() &&
           MI.registerDefIsDead(X86::EFLAGS, TRI);
  case X86::LEA64r:
    return getAccessBase(MI) == Reg &&
           TRI->getEncodingValue(MI.getOperand(0).getReg()) < 16;
  default:
    return false;
  }
}

// Whether MI only reads memory through Reg, as the base of its memory operand
// with no segment and any index but Reg: a load, neither volatile nor atomic,
// that reads Reg nowhere else, so that the address in Reg does not escape.
bool X86WindowsImportFusion::isValueRead(const MachineInstr &MI,
                                         Register Reg) const {
  if (!MI.mayLoad() || MI.mayStore() || MI.isCall() || MI.isBranch() ||
      MI.hasUnmodeledSideEffects() || MI.isBundled() ||
      MI.memoperands_empty() ||
      any_of(MI.memoperands(), [](const MachineMemOperand *MMO) {
        return MMO->isVolatile() || MMO->isAtomic();
      }))
    return false;
  const MCInstrDesc &Desc = MI.getDesc();
  int MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
  if (MemOp < 0)
    return false;
  MemOp += X86II::getOperandBias(Desc);
  const MachineOperand &Base = MI.getOperand(MemOp + X86::AddrBaseReg);
  if (!Base.isReg() || Base.getReg() != Reg ||
      MI.getOperand(MemOp + X86::AddrSegmentReg).getReg() ||
      !MI.getOperand(MemOp + X86::AddrDisp).isImm())
    return false;
  for (const auto &[I, MO] : enumerate(MI.operands()))
    if (I != unsigned(MemOp + X86::AddrBaseReg) && MO.isReg() && MO.getReg() &&
        MO.readsReg() && TRI->regsOverlap(MO.getReg(), Reg))
      return false;
  return true;
}

bool X86WindowsImportFusion::run(MachineFunction &MF,
                                 function_ref<ReachingDefInfo &()> GetRDI) {
  const X86Subtarget &ST = MF.getSubtarget<X86Subtarget>();
  // A thunk must make every indirect call, and LVI hardening fences each load
  // it leaves. Import call optimization wants each call through the pointer.
  // An outlined function does not say which registers its callers read once
  // it returns.
  if (ST.useIndirectThunkCalls() || ST.useLVILoadHardening() ||
      MF.getFunction().getParent()->getModuleFlag("import-call-optimization") ||
      MF.isOutlined())
    return false;
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  SmallVector<MachineInstr *, 8> Loads, MemCalls;
  bool HasRegCalls = false, HasDataLoads = false;
  // The globals whose import pointers an instruction other than a load of
  // one into a register names.
  SmallPtrSet<const GlobalValue *, 4> OtherUses;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      const MachineOperand *Pointer = getLoadedPointer(MI);
      if (MI.isBundled() || !Pointer)
        for (const MachineOperand &MO : MI.operands())
          if (MO.isGlobal() && (MO.getTargetFlags() == X86II::MO_DLLIMPORT ||
                                MO.getTargetFlags() == X86II::MO_COFFSTUB))
            OtherUses.insert(MO.getGlobal());
      if (MI.isBundled())
        continue;
      if (Pointer) {
        Loads.push_back(&MI);
        HasDataLoads |= isDataPointer(*Pointer);
      } else if (getUnfoldedOpcode(MI.getOpcode()) && getImportPointer(MI, 0))
        MemCalls.push_back(&MI);
      else if (getFoldedOpcode(MI.getOpcode()))
        HasRegCalls = true;
    }
  }
  if (Loads.empty() || (!HasRegCalls && MemCalls.empty() && !HasDataLoads))
    return false;

  // Collected before anything changes, which leaves the reaching definitions
  // describing code that is no longer there.
  struct Fusion {
    MachineInstr *Load;
    SmallVector<MachineInstr *, 4> RegCalls;
    SmallVector<MachineInstr *, 2> MemCalls;
    // The blocks after the load's on the paths to MemCalls.
    SmallVector<MachineBasicBlock *, 4> MemCallBlocks;
    // Whether only the calls, or for a load of the pointer of data the
    // accesses, read the load's register.
    bool OnlyCalls;
    // For a load of the pointer of data, the accesses through its register,
    // which are all that read it, or the add of a constant to it.
    SmallVector<MachineInstr *, 4> Accesses;
    MachineInstr *Add = nullptr;
  };
  SmallVector<Fusion, 4> Work;
  SmallSetVector<MachineInstr *, 4> ValueOnly;
  SmallPtrSet<MachineInstr *, 4> FusedMemCalls;
  ReachingDefInfo *RDI = HasRegCalls || HasDataLoads ? &GetRDI() : nullptr;
  for (MachineInstr *Load : Loads) {
    Register Reg = Load->getOperand(0).getReg();
    const MachineOperand &Pointer = *getLoadedPointer(*Load);
    Fusion F{Load, {}, {}, {}, false, {}, nullptr};

    // A call through the register is described where the load is the only
    // definition that reaches it; any other use, a call that also passes the
    // register as an argument among them, keeps the load.
    SmallPtrSet<MachineInstr *, 4> Uses;
    if (RDI)
      RDI->getGlobalUses(Load, Reg, Uses);
    F.OnlyCalls = !Uses.empty();
    for (MachineInstr *Use : Uses) {
      if (!canGoThrough(Use->getOpcode(), Reg, TII, TRI) || Use->isBundled() ||
          !Use->getOperand(0).isReg() || Use->getOperand(0).getReg() != Reg ||
          !isInSameSection(*Load, *Use) ||
          RDI->getUniqueReachingMIDef(Use, Reg) != Load) {
        F.OnlyCalls = false;
        continue;
      }
      F.RegCalls.push_back(Use);
      if (any_of(drop_begin(Use->operands()), [&](const MachineOperand &MO) {
            return MO.isReg() && MO.isUse() && MO.getReg() &&
                   TRI->regsOverlap(MO.getReg(), Reg);
          }))
        F.OnlyCalls = false;
    }

    // A call through the pointer that the register still holds may read the
    // register instead, such as one that a test of the pointer guards.
    for (MachineInstr *Call : MemCalls) {
      if (FusedMemCalls.contains(Call) ||
          !canGoThrough(getUnfoldedOpcode(Call->getOpcode()), Reg, TII,
                        TRI) ||
          !isSamePointer(*getImportPointer(*Call, 0), Pointer) ||
          !isInSameSection(*Load, *Call))
        continue;
      SmallVector<MachineBasicBlock *, 8> Blocks;
      if (!reachesUnmodified(*Load, *Call, Reg, Blocks))
        continue;
      F.MemCalls.push_back(Call);
      FusedMemCalls.insert(Call);
      append_range(F.MemCallBlocks, Blocks);
    }
    if (!F.RegCalls.empty() || !F.MemCalls.empty()) {
      Work.push_back(std::move(F));
      continue;
    }

    // Each access through the register of a load of the pointer of data that
    // the load alone reaches is described; the linker removes the load only
    // where they are all that read it. A load whose register one add reads
    // takes the add's sum.
    if (!isDataPointer(Pointer) || Uses.empty())
      continue;
    // Where only reads of memory read the address of data that never
    // changes, the address may be that of the image's own copy.
    if (isImmutableData(Pointer) &&
        all_of(Uses, [&](MachineInstr *Use) { return isValueRead(*Use, Reg); }))
      ValueOnly.insert(Load);
    auto IsFused = [&](MachineInstr *Use) {
      return !Use->isBundled() && isInSameSection(*Load, *Use) &&
             RDI->getUniqueReachingMIDef(Use, Reg) == Load;
    };
    MachineInstr *Only = *Uses.begin();
    if (Uses.size() == 1 && IsFused(Only) && isFusibleAdd(*Only, Reg) &&
        Only->getParent() == Load->getParent()) {
      F.Add = Only;
      Work.push_back(std::move(F));
      continue;
    }
    F.OnlyCalls = true;
    for (MachineInstr *Use : Uses) {
      if (IsFused(Use) && isFusibleAccess(*Use, Reg))
        F.Accesses.push_back(Use);
      else
        F.OnlyCalls = false;
    }
    // Where the load stays, a RIP-relative access saves only waiting for it,
    // at the cost of a longer encoding, which pays only next to the load,
    // not where a loop the load was hoisted out of repeats the access.
    if (!F.OnlyCalls)
      llvm::erase_if(F.Accesses, [&](MachineInstr *Access) {
        return Access->getParent() != Load->getParent();
      });
    if (!F.Accesses.empty())
      Work.push_back(std::move(F));
  }
  // Where the function only reads data that never changes through its
  // address, or compares addresses within it, every load of the data's import
  // pointer may read the image's own copy, all of them together, so that
  // addresses that different loads lead to still compare as before.
  DenseMap<const GlobalValue *, bool> ReadOnlyThrough;
  for (MachineInstr *Load : Loads) {
    const MachineOperand &Pointer = *getLoadedPointer(*Load);
    if (!isImmutableData(Pointer) ||
        Pointer.getTargetFlags() != X86II::MO_DLLIMPORT)
      continue;
    const GlobalValue *GV = Pointer.getGlobal();
    auto [It, Inserted] = ReadOnlyThrough.try_emplace(GV, false);
    if (Inserted) {
      It->second = !OtherUses.contains(GV) &&
                   !globalAddressMayBeCapturedIn(GV, MF.getFunction());
      LLVM_DEBUG(dbgs() << "Loads of the import pointer of " << GV->getName()
                        << (It->second ? " read only through it\n"
                                       : " may give its address away\n"));
    }
    if (It->second)
      ValueOnly.insert(Load);
  }
  X86MachineFunctionInfo *FI = MF.getInfo<X86MachineFunctionInfo>();
  for (MachineInstr *Load : ValueOnly)
    FI->addImportValueOnlyLoad(Load);
  NumValueOnly += ValueOnly.size();
  if (Work.empty())
    return !ValueOnly.empty();

  SmallVector<MachineBasicBlock *, 8> LiveInBlocks;
  using UseKind = X86MachineFunctionInfo::ImportFusedUseKind;
  for (Fusion &F : Work) {
    MachineInstr *Load = F.Load;
    Register Reg = Load->getOperand(0).getReg();
    if (F.Add) {
      FI->addImportFusedUse(F.Add, Load, Reg, UseKind::Add);
      FI->addImportFusedLoad(Load, false);
      ++NumAdds;
      continue;
    }
    if (!F.Accesses.empty()) {
      for (MachineInstr *Access : F.Accesses)
        FI->addImportFusedUse(Access, Load, Reg, UseKind::Access);
      NumAccesses += F.Accesses.size();
      if (F.OnlyCalls)
        undefDebugValues(*Load, Reg);
      FI->addImportFusedLoad(Load, F.OnlyCalls);
      continue;
    }
    // A load that serves one call, in its own block, is executed as often as
    // the call, so holding the pointer saves nothing: the load goes, and the
    // call is what a compiler that never hoisted it would have emitted. One
    // that serves several calls saves a load for each but the first where the
    // function is in another image, so the linker decides.
    bool Pointless = F.OnlyCalls && F.MemCalls.empty() &&
                     F.RegCalls.size() == 1 &&
                     F.RegCalls.front()->getParent() == Load->getParent();
    for (MachineInstr *Call : F.RegCalls) {
      if (Pointless) {
        foldCall(*Call, *Load);
        continue;
      }
      FI->addImportFusedUse(Call, Load, Reg, UseKind::Call);
      noteCallee(*Call, *Load);
      ++NumFused;
    }
    // The register now lives on to each call through the pointer that
    // becomes a call through it, past uses that killed it.
    for (MachineInstr *Call : F.MemCalls) {
      MachineInstr *New = callThroughRegister(*Call, Reg);
      FI->addImportFusedUse(New, Load, Reg, UseKind::Call);
      noteCallee(*New, *Load);
      ++NumFused;
    }
    if (!F.MemCalls.empty()) {
      for (MachineInstr &MI : make_range(std::next(Load->getIterator()),
                                         Load->getParent()->instr_end()))
        MI.clearRegisterKills(Reg, TRI);
      for (MachineBasicBlock *MBB : F.MemCallBlocks)
        for (MachineInstr &MI : MBB->instrs())
          MI.clearRegisterKills(Reg, TRI);
      append_range(LiveInBlocks, F.MemCallBlocks);
    }
    if (Pointless) {
      undefDebugValues(*Load, Reg);
      Load->eraseFromParent();
      ++NumDeleted;
      continue;
    }
    if (F.OnlyCalls) {
      moveOffBlockEnd(*Load, Reg);
      undefDebugValues(*Load, Reg);
    }
    FI->addImportFusedLoad(Load, F.OnlyCalls);
  }
  if (!LiveInBlocks.empty())
    fullyRecomputeLiveIns(LiveInBlocks);
  return true;
}

bool X86WindowsImportFusionLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  // The reaching definitions are computed only for a function with something
  // to fuse.
  std::optional<ReachingDefInfo> RDI;
  return X86WindowsImportFusion().run(MF, [&]() -> ReachingDefInfo & {
    if (!RDI) {
      RDI.emplace();
      RDI->run(MF);
    }
    return *RDI;
  });
}

PreservedAnalyses
X86WindowsImportFusionPass::run(MachineFunction &MF,
                                MachineFunctionAnalysisManager &MFAM) {
  bool Changed = X86WindowsImportFusion().run(MF, [&]() -> ReachingDefInfo & {
    return MFAM.getResult<ReachingDefAnalysis>(MF);
  });
  if (!Changed)
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses().preserveSet<CFGAnalyses>();
}
