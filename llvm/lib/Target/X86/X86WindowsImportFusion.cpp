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
// A thread-local variable that may be in another image is reached through
// the import pointer of its record: the address of the image's TLS index, then
// the variable's offset in the image's TLS template. Where the record turns
// out to be in the image, the linker knows both, and makes the access the one
// a variable of the image gets. Each load from the record is described as an
// access, and as a load with the instructions that read its register: those
// that load the index through the address, and those that add the offset to
// another register in their memory operand.
//
// A load of the import pointer of data whose pointer a load of the import
// pointer of other data left in a register on every path to it is described
// with that load: where the linker finds both data in one other image, whose
// layout it knows, the load may become an lea of the distance between them
// from that register.
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
STATISTIC(NumAnchored, "Number of loads described with loads that may give "
                      "their pointers");

namespace {

class X86WindowsImportFusion {
  const TargetInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;

  // An instruction and the register through which it reads a load's value.
  using Reader = std::pair<MachineInstr *, Register>;
  bool getReaders(MachineInstr &Load, Register Reg, ReachingDefInfo &RDI,
                  SmallVectorImpl<Reader> &Readers,
                  SmallVectorImpl<Reader> &Copies) const;

  void foldCall(MachineInstr &Call, const MachineInstr &Load);
  MachineInstr *callThroughRegister(MachineInstr &Call, Register Reg);
  bool reachesUnmodified(MachineInstr &Load, MachineInstr &Call, Register Reg,
                         SmallVectorImpl<MachineBasicBlock *> &Blocks) const;
  void undefDebugValues(MachineInstr &Load, Register Reg) const;
  void moveOffBlockEnd(MachineInstr &Load, Register Reg) const;
  bool isFusibleAccess(const MachineInstr &MI, Register Reg) const;
  bool isFusibleIndex(const MachineInstr &MI, Register Reg) const;
  bool isFusibleAdd(const MachineInstr &MI, Register Reg) const;
  bool isValueRead(const MachineInstr &MI, Register Reg) const;
#ifdef EXPENSIVE_CHECKS
  void verify(MachineFunction &MF) const;
#endif

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

// Whether Pointer names the record of a thread-local variable, <name>$tls,
// which the back end reaches through its import pointer by name and C++
// code through a declaration.
static bool isThreadLocalRecord(const MachineOperand &Pointer) {
  return (Pointer.isSymbol() ? StringRef(Pointer.getSymbolName())
                             : Pointer.getGlobal()->getName())
      .ends_with("$tls");
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
      !(Disp.isGlobal() || Disp.isSymbol()) || Disp.getOffset() != 0)
    return nullptr;
  unsigned Flags = Disp.getTargetFlags();
  if (Flags != X86II::MO_DLLIMPORT &&
      (Flags != X86II::MO_COFFSTUB || Disp.isSymbol()))
    return nullptr;
  if (Disp.isSymbol() && !isThreadLocalRecord(Disp))
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
  if (Pointer.isSymbol())
    return true;
  const GlobalValue *GV = Pointer.getGlobal();
  if (const auto *GA = dyn_cast<GlobalAlias>(GV))
    GV = GA->getAliaseeObject();
  return GV && !isa<Function>(GV);
}

// Whether Pointer is the import pointer of data that never changes and whose
// every definition holds the same value, so that a copy of it in any image
// reads as the data itself does.
static bool isImmutableData(const MachineOperand &Pointer) {
  if (Pointer.isSymbol())
    return false;
  const auto *GV =
      dyn_cast_or_null<GlobalVariable>(Pointer.getGlobal()->getAliaseeObject());
  return GV && GV->isConstant() && GV->hasDefinitiveInitializer() &&
         (GV->isWeakForLinker() || GV->hasAvailableExternallyLinkage());
}

static bool isSamePointer(const MachineOperand &A, const MachineOperand &B) {
  if (A.isSymbol() || B.isSymbol())
    return A.isSymbol() && B.isSymbol() &&
           StringRef(A.getSymbolName()) == B.getSymbolName();
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
  if (!Pointer.isGlobal())
    return;
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

// Collects the instructions that read the value Load gives Reg, each with the
// register it reads and reached by no other value, following the copies of
// the value from register to register, which the register allocator makes
// around a call, into Copies. Where the linker deletes Load, a copy copies
// nothing that a reader still reads. Returns whether Readers holds every
// instruction that reads the value; where a copy may copy another value too,
// none is followed, and the copies are among the readers.
bool X86WindowsImportFusion::getReaders(MachineInstr &Load, Register Reg,
                                        ReachingDefInfo &RDI,
                                        SmallVectorImpl<Reader> &Readers,
                                        SmallVectorImpl<Reader> &Copies) const {
  SmallPtrSet<MachineInstr *, 4> Defs = {&Load}, Seen;
  SmallVector<Reader, 4> Work = {{&Load, Reg}}, Uses;
  while (!Work.empty()) {
    auto [Def, R] = Work.pop_back_val();
    SmallPtrSet<MachineInstr *, 4> DefUses;
    RDI.getGlobalUses(Def, R, DefUses);
    for (MachineInstr *Use : DefUses) {
      if (Use->getOpcode() == X86::MOV64rr && !Use->isBundled() &&
          Use->getOperand(1).getReg() == R &&
          Use->getOperand(0).getReg() != R) {
        if (Defs.insert(Use).second) {
          Copies.push_back({Use, R});
          Work.push_back({Use, Use->getOperand(0).getReg()});
        }
      } else if (Seen.insert(Use).second) {
        Uses.push_back({Use, R});
      }
    }
  }
  auto IsReached = [&](const Reader &U) {
    SmallPtrSet<MachineInstr *, 2> Reaching;
    RDI.getGlobalReachingDefs(U.first, U.second, Reaching);
    return !Reaching.empty() &&
           all_of(Reaching, [&](MachineInstr *D) { return Defs.contains(D); });
  };
  if (!all_of(Copies, IsReached)) {
    Copies.clear();
    Defs = {&Load};
    Uses.clear();
    SmallPtrSet<MachineInstr *, 4> DefUses;
    RDI.getGlobalUses(&Load, Reg, DefUses);
    for (MachineInstr *Use : DefUses)
      Uses.push_back({Use, Reg});
  }
  for (const Reader &U : Uses)
    if (IsReached(U))
      Readers.push_back(U);
  return Readers.size() == Uses.size();
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
// neither reads nor writes its register costs nothing. A load that the linker
// may make an lea from another load's register reads that register.
void X86WindowsImportFusion::moveOffBlockEnd(MachineInstr &Load,
                                             Register Reg) const {
  MachineBasicBlock *MBB = Load.getParent();
  if (next_nodbg(Load.getIterator(), MBB->instr_end()) != MBB->instr_end() ||
      Load.getIterator() == MBB->instr_begin())
    return;
  auto Prev = prev_nodbg(Load.getIterator(), MBB->instr_begin());
  const auto &Anchors =
      MBB->getParent()->getInfo<X86MachineFunctionInfo>()->getImportAnchors();
  auto Anchored = Anchors.find(&*Prev);
  if (Prev->isMetaInstruction() || Prev->isPseudo() || Prev->isBundled() ||
      Prev->readsRegister(Reg, TRI) || Prev->modifiesRegister(Reg, TRI) ||
      Prev->getFlag(MachineInstr::FrameSetup) ||
      Prev->getFlag(MachineInstr::FrameDestroy) ||
      (Anchored != Anchors.end() &&
       any_of(Anchored->second, [&](const auto &Anchor) {
         return TRI->regsOverlap(Anchor.second, Reg);
       })))
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

// Whether MI reads or writes memory, or computes an address, with a memory
// operand that adds Reg, with scale 1, to another register below the first
// sixteen, in a legacy or VEX encoding outside APX's map 4, and reads Reg
// nowhere else: the linker adds the value in Reg to the displacement and
// leaves Reg out. Or whether MI adds Reg to another such register, which the
// linker makes an add of the value; or, with the flags dead, adds another
// such register to Reg, which it makes an lea of the other plus the value.
bool X86WindowsImportFusion::isFusibleIndex(const MachineInstr &MI,
                                            Register Reg) const {
  if (MI.getOpcode() == X86::ADD64rr) {
    Register Dst = MI.getOperand(0).getReg(), Src = MI.getOperand(2).getReg();
    return !MI.isBundled() && TRI->getEncodingValue(Dst) < 16 &&
           TRI->getEncodingValue(Src) < 16 && !TRI->regsOverlap(Dst, Src) &&
           (Src == Reg ||
            (Dst == Reg && MI.registerDefIsDead(X86::EFLAGS, TRI)));
  }
  const MCInstrDesc &Desc = MI.getDesc();
  uint64_t Encoding = Desc.TSFlags & X86II::EncodingMask;
  int MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
  if ((Encoding != X86II::LEGACY && Encoding != X86II::VEX) ||
      (Desc.TSFlags & X86II::OpMapMask) == X86II::T_MAP4 || MI.isBundled() ||
      MI.isPseudo() || MemOp < 0 || TRI->getEncodingValue(Reg) >= 16)
    return false;
  MemOp += X86II::getOperandBias(Desc);
  Register Base = MI.getOperand(MemOp + X86::AddrBaseReg).getReg();
  Register Index = MI.getOperand(MemOp + X86::AddrIndexReg).getReg();
  Register Other = Base == Reg ? Index : Index == Reg ? Base : Register();
  if (!Other || Other == X86::RIP || TRI->regsOverlap(Other, Reg) ||
      TRI->getEncodingValue(Other) >= 16 ||
      MI.getOperand(MemOp + X86::AddrScaleAmt).getImm() != 1 ||
      MI.getOperand(MemOp + X86::AddrSegmentReg).getReg() ||
      !MI.getOperand(MemOp + X86::AddrDisp).isImm())
    return false;
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

  // The loads of the import pointers of data that the register of a load of
  // another such pointer, the nearest earlier ones first, still holds the
  // pointer of, each with that load. An lea from RSP or R12 needs a SIB byte,
  // and one from a register past the first sixteen REX2, which the load has
  // no room for.
  SmallVector<std::pair<MachineInstr *, MachineInstr *>, 4> Anchors;
  SmallVector<MachineInstr *, 8> DataLoads;
  for (MachineInstr *Load : Loads)
    if (isDataPointer(*getLoadedPointer(*Load)) &&
        !isThreadLocalRecord(*getLoadedPointer(*Load)))
      DataLoads.push_back(Load);
  for (auto [I, Load] : enumerate(DataLoads)) {
    const MachineOperand &Pointer = *getLoadedPointer(*Load);
    SmallVector<Register, 4> Regs;
    unsigned Tried = 0;
    for (MachineInstr *Anchor : reverse(ArrayRef(DataLoads).take_front(I))) {
      Register Reg = Anchor->getOperand(0).getReg();
      if (Regs.size() == 4 || Tried == 16)
        break;
      if (isSamePointer(*getLoadedPointer(*Anchor), Pointer) ||
          Reg == X86::RSP || Reg == X86::R12 ||
          TRI->getEncodingValue(Reg) >= 16 || is_contained(Regs, Reg) ||
          !isInSameSection(*Anchor, *Load))
        continue;
      ++Tried;
      SmallVector<MachineBasicBlock *, 8> Blocks;
      if (!reachesUnmodified(*Anchor, *Load, Reg, Blocks))
        continue;
      Regs.push_back(Reg);
      Anchors.push_back({Load, Anchor});
    }
  }

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
    // For a load of the pointer of data, the accesses through its register
    // or a copy of it, each with the register it reads, which are all that
    // read it, with the copies; or the add of a constant to it.
    SmallVector<Reader, 4> Accesses, Copies;
    MachineInstr *Add = nullptr;
    // For a load of the pointer of a thread-local variable's record, the
    // accesses that load from the record and the instructions that read what
    // each loads: accesses through the address of the TLS index, or memory
    // operands that add an offset; with whether they alone read it.
    struct Chain {
      MachineInstr *Load;
      Register Reg;
      bool Index;
      bool OnlyUses;
      SmallVector<Reader, 4> Uses, Copies;
    };
    SmallVector<Chain, 2> Chains;
  };
  SmallVector<Fusion, 4> Work;
  SmallSetVector<MachineInstr *, 4> ValueOnly;
  SmallPtrSet<MachineInstr *, 4> FusedMemCalls;
  ReachingDefInfo *RDI = HasRegCalls || HasDataLoads ? &GetRDI() : nullptr;
  for (MachineInstr *Load : Loads) {
    Register Reg = Load->getOperand(0).getReg();
    const MachineOperand &Pointer = *getLoadedPointer(*Load);
    Fusion F{Load, {}, {}, {}, false, {}, {}, nullptr, {}};

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
    MachineInstr *Only = *Uses.begin();
    if (Uses.size() == 1 && !Only->isBundled() &&
        isInSameSection(*Load, *Only) &&
        RDI->getUniqueReachingMIDef(Only, Reg) == Load &&
        isFusibleAdd(*Only, Reg) && Only->getParent() == Load->getParent()) {
      F.Add = Only;
      Work.push_back(std::move(F));
      continue;
    }
    // The accesses are found through the copies of the load's value, each
    // through the register it reads.
    SmallVector<Reader, 4> Readers;
    F.OnlyCalls = getReaders(*Load, Reg, *RDI, Readers, F.Copies);
    for (auto [Use, R] : Readers) {
      if (!Use->isBundled() && isInSameSection(*Load, *Use) &&
          isFusibleAccess(*Use, R))
        F.Accesses.push_back({Use, R});
      else
        F.OnlyCalls = false;
    }
    // Where the load stays, a RIP-relative access saves only waiting for it,
    // at the cost of a longer encoding, which pays only next to the load,
    // not where a loop the load was hoisted out of repeats the access.
    if (!F.OnlyCalls)
      llvm::erase_if(F.Accesses, [&](const Reader &Access) {
        return Access.first->getParent() != Load->getParent();
      });
    // The record's first word is the address of the TLS index, and its next
    // two the 32-bit offsets of the variable and its guard. An offset is
    // described only where its every reader adds it in a memory operand, since
    // the linker otherwise keeps the register.
    if (isThreadLocalRecord(Pointer)) {
      for (auto [Access, AccessReg] : F.Accesses) {
        unsigned Opcode = Access->getOpcode();
        if (Opcode != X86::MOV32rm && Opcode != X86::MOV64rm)
          continue;
        int64_t Disp = Access->getOperand(1 + X86::AddrDisp).getImm();
        bool Index = Opcode == X86::MOV32rm && (Disp == 8 || Disp == 12);
        if (!Index && (Opcode != X86::MOV64rm || Disp != 0))
          continue;
        Register Def = Access->getOperand(0).getReg();
        Register Reg = Def;
        if (Index)
          Reg =
              TRI->getMatchingSuperReg(Def, X86::sub_32bit, &X86::GR64RegClass);
        // The 64-bit register, which blocks list as live-in.
        Fusion::Chain C{Access, Reg, Index, true, {}, {}};
        SmallVector<Reader, 4> Readers;
        C.OnlyUses = getReaders(*Access, Reg, *RDI, Readers, C.Copies);
        for (auto [Use, R] : Readers) {
          if (!Use->isBundled() && isInSameSection(*Access, *Use) &&
              (Index ? isFusibleIndex(*Use, R) : isFusibleAccess(*Use, R)))
            C.Uses.push_back({Use, R});
          else
            C.OnlyUses = false;
        }
        if (!C.OnlyUses)
          llvm::erase_if(C.Uses, [&](const Reader &Use) {
            return Index || Use.first->getParent() != Access->getParent();
          });
        if (!C.Uses.empty())
          F.Chains.push_back(std::move(C));
      }
    }
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
  // Where a load of such data stays for what else reads its register, each
  // access described with it that only reads may read the copy directly.
  for (Fusion &F : Work) {
    const MachineOperand &Pointer = *getLoadedPointer(*F.Load);
    if (F.Accesses.empty() || ValueOnly.contains(F.Load) ||
        !isImmutableData(Pointer) ||
        Pointer.getTargetFlags() != X86II::MO_DLLIMPORT)
      continue;
    for (auto [Access, Reg] : F.Accesses)
      if (isValueRead(*Access, Reg))
        ValueOnly.insert(Access);
  }
  X86MachineFunctionInfo *FI = MF.getInfo<X86MachineFunctionInfo>();
  for (MachineInstr *Load : ValueOnly)
    FI->addImportValueOnlyLoad(Load);
  NumValueOnly += ValueOnly.size();
  for (auto [Load, Anchor] : Anchors)
    FI->addImportAnchor(Load, Anchor, Anchor->getOperand(0).getReg());
  NumAnchored += Anchors.size();
  if (Work.empty()) {
#ifdef EXPENSIVE_CHECKS
    verify(MF);
#endif
    return !ValueOnly.empty() || !Anchors.empty();
  }

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
      for (auto [Access, R] : F.Accesses)
        FI->addImportFusedUse(Access, Load, R, UseKind::Access);
      NumAccesses += F.Accesses.size();
      for (Fusion::Chain &C : F.Chains) {
        for (auto [Use, R] : C.Uses)
          FI->addImportFusedUse(Use, C.Load, R,
                                C.Index ? UseKind::Index : UseKind::Access);
        NumAccesses += C.Uses.size();
        if (C.OnlyUses) {
          undefDebugValues(*C.Load, C.Reg);
          for (auto [Copy, R] : C.Copies)
            undefDebugValues(*Copy, Copy->getOperand(0).getReg());
        }
        FI->addImportFusedLoad(C.Load, C.OnlyUses);
      }
      if (F.OnlyCalls) {
        undefDebugValues(*Load, Reg);
        for (auto [Copy, R] : F.Copies)
          undefDebugValues(*Copy, Copy->getOperand(0).getReg());
      }
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
#ifdef EXPENSIVE_CHECKS
  verify(MF);
#endif
  return true;
}

#ifdef EXPENSIVE_CHECKS
// The linker rewrites a described instruction as though the register it reads
// held only what the load it names gave it, which MC can check only where
// execution falls through from the load. Reaching definitions computed afresh
// check it on every path, through copies for an access or index, once the
// pass has changed the code; and that an anchor's register still holds what
// its load gave it.
void X86WindowsImportFusion::verify(MachineFunction &MF) const {
  const X86MachineFunctionInfo *FI = MF.getInfo<X86MachineFunctionInfo>();
  ReachingDefInfo RDI;
  RDI.run(MF);
  using UseKind = X86MachineFunctionInfo::ImportFusedUseKind;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      auto Use = FI->getImportFusedUses().find(&MI);
      if (Use != FI->getImportFusedUses().end()) {
        auto [Load, Reg, Kind] = Use->second;
        bool Copies = Kind == UseKind::Access || Kind == UseKind::Index;
        SmallVector<std::pair<MachineInstr *, Register>, 4> Work = {
            {&MI, Reg}};
        SmallPtrSet<MachineInstr *, 4> Seen;
        while (!Work.empty()) {
          auto [Reader, R] = Work.pop_back_val();
          SmallPtrSet<MachineInstr *, 2> Defs;
          RDI.getGlobalReachingDefs(Reader, R, Defs);
          assert(!Defs.empty() && "fused instruction reads a value from entry");
          for (MachineInstr *Def : Defs) {
            if (Def == Load || !Seen.insert(Def).second)
              continue;
            assert(Copies && Def->getOpcode() == X86::MOV64rr &&
                   "fused instruction reads a value its load did not give");
            Work.push_back({Def, Def->getOperand(1).getReg()});
          }
        }
      }
      auto Anchored = FI->getImportAnchors().find(&MI);
      if (Anchored == FI->getImportAnchors().end())
        continue;
      for (auto [Anchor, Reg] : Anchored->second) {
        SmallVector<MachineBasicBlock *, 8> Blocks;
        assert(reachesUnmodified(const_cast<MachineInstr &>(*Anchor), MI, Reg,
                                 Blocks) &&
               "anchor's register no longer holds its load's pointer");
      }
    }
  }
}
#endif

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
