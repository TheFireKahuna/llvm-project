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
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/Module.h"

using namespace llvm;

#define DEBUG_TYPE "x86-windows-import-fusion"

STATISTIC(NumFused, "Number of calls described as fused with a load");
STATISTIC(NumDeleted, "Number of import pointer loads folded into a call");

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

bool X86WindowsImportFusion::run(MachineFunction &MF,
                                 function_ref<ReachingDefInfo &()> GetRDI) {
  const X86Subtarget &ST = MF.getSubtarget<X86Subtarget>();
  // A thunk must make every indirect call, and LVI hardening fences each load
  // it leaves. Import call optimization wants each call through the pointer.
  if (ST.useIndirectThunkCalls() || ST.useLVILoadHardening() ||
      MF.getFunction().getParent()->getModuleFlag("import-call-optimization"))
    return false;
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  SmallVector<MachineInstr *, 8> Loads, MemCalls;
  bool HasRegCalls = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      if (MI.isBundled())
        continue;
      if (getLoadedPointer(MI))
        Loads.push_back(&MI);
      else if (getUnfoldedOpcode(MI.getOpcode()) && getImportPointer(MI, 0))
        MemCalls.push_back(&MI);
      else if (getFoldedOpcode(MI.getOpcode()))
        HasRegCalls = true;
    }
  }
  if (Loads.empty() || (!HasRegCalls && MemCalls.empty()))
    return false;

  // Collected before anything changes, which leaves the reaching definitions
  // describing code that is no longer there.
  struct Fusion {
    MachineInstr *Load;
    SmallVector<MachineInstr *, 4> RegCalls;
    SmallVector<MachineInstr *, 2> MemCalls;
    // The blocks after the load's on the paths to MemCalls.
    SmallVector<MachineBasicBlock *, 4> MemCallBlocks;
    // Whether only the calls read the load's register.
    bool OnlyCalls;
  };
  SmallVector<Fusion, 4> Work;
  SmallPtrSet<MachineInstr *, 4> FusedMemCalls;
  ReachingDefInfo *RDI = HasRegCalls ? &GetRDI() : nullptr;
  for (MachineInstr *Load : Loads) {
    Register Reg = Load->getOperand(0).getReg();
    const MachineOperand &Pointer = *getLoadedPointer(*Load);
    Fusion F{Load, {}, {}, {}, false};

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
    if (!F.RegCalls.empty() || !F.MemCalls.empty())
      Work.push_back(std::move(F));
  }
  if (Work.empty())
    return false;

  X86MachineFunctionInfo *FI = MF.getInfo<X86MachineFunctionInfo>();
  SmallVector<MachineBasicBlock *, 8> LiveInBlocks;
  for (Fusion &F : Work) {
    MachineInstr *Load = F.Load;
    Register Reg = Load->getOperand(0).getReg();
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
      FI->addImportFusedCall(Call, Load, Reg);
      noteCallee(*Call, *Load);
      ++NumFused;
    }
    // The register now lives on to each call through the pointer that
    // becomes a call through it, past uses that killed it.
    for (MachineInstr *Call : F.MemCalls) {
      MachineInstr *New = callThroughRegister(*Call, Reg);
      FI->addImportFusedCall(New, Load, Reg);
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
