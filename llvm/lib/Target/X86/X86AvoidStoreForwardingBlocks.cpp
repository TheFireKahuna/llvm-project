//===- X86AvoidStoreForwardingBlocks.cpp - Avoid HW Store Forward Block ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// If a load follows a store and reloads data that the store has written to
// memory, Intel microarchitectures can in many cases forward the data directly
// from the store to the load, This "store forwarding" saves cycles by enabling
// the load to directly obtain the data instead of accessing the data from
// cache or memory.
// A "store forward block" occurs in cases that a store cannot be forwarded to
// the load. The most typical case of store forward block on Intel Core
// microarchitecture that a small store cannot be forwarded to a large load.
// The estimated penalty for a store forward block is ~13 cycles.
//
// This pass tries to recognize and handle cases where "store forward block"
// is created by the compiler when lowering memcpy calls to a sequence
// of a load and a store.
//
// The pass currently only handles cases where memcpy is lowered to
// XMM/YMM registers, it tries to break the memcpy into smaller copies.
// Only non-atomic, non-volatile accesses may be split. Potentially overlapping
// copies keep all loads before the stores and require register headroom.
//
// It could be better for performance to solve the problem by loading
// to XMM/YMM then inserting the partial store before storing back from XMM/YMM
// to memory, but this will result in a more conservative optimization since it
// requires we prove that all memory accesses between the blocking store and the
// load must alias/don't alias before we can move the store, whereas the
// transformation done here is correct regardless to other memory accesses.
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/RegisterPressure.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/Support/MathExtras.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "x86-avoid-sfb"

static cl::opt<bool> DisableX86AvoidStoreForwardBlocks(
    "x86-disable-avoid-SFB", cl::Hidden,
    cl::desc("X86: Disable Store Forwarding Blocks fixup."), cl::init(false));

static cl::opt<unsigned> X86AvoidSFBInspectionLimit(
    "x86-sfb-inspection-limit",
    cl::desc("X86: Number of instructions backward to "
             "inspect for store forwarding blocks."),
    cl::init(20), cl::Hidden);

namespace {

using DisplacementSizeMap = std::map<int64_t, unsigned>;

class X86AvoidSFBImpl {
public:
  X86AvoidSFBImpl(AliasAnalysis *AA) : AA(AA) {};
  bool runOnMachineFunction(MachineFunction &MF);

private:
  MachineRegisterInfo *MRI = nullptr;
  const X86InstrInfo *TII = nullptr;
  const X86RegisterInfo *TRI = nullptr;
  SmallVector<std::pair<MachineInstr *, MachineInstr *>, 2>
      BlockedLoadsStoresPairs;
  AliasAnalysis *AA = nullptr;
  std::unique_ptr<LiveVariables> LV;
  RegisterClassInfo RCI;
  RegionPressure Pressure;
  RegPressureTracker Tracker{Pressure};
  const MachineBasicBlock *PressureMBB = nullptr;
  struct CopyPiece {
    unsigned LoadOpcode, StoreOpcode;
    unsigned Size;
    int64_t Offset;
    Register ForwardedReg = 0;
  };
  struct CopyPlan {
    MachineInstr *Load, *Store;
    bool IsDisjoint;
    SmallVector<CopyPiece, 4> Pieces;
  };

  bool selectCopyRegisters(CopyPlan &Plan);
  void emitCopy(const CopyPlan &Plan, const CopyPiece &Piece,
                MachineInstr *&LastLoad, MachineInstr *&LastStore);

  /// Returns couples of Load then Store to memory which look
  ///  like a memcpy.
  void findPotentiallylBlockedCopies(MachineFunction &MF);
  /// Plan smaller copies that do not straddle the blocking stores.
  void planCopies(CopyPlan &Plan,
                  const DisplacementSizeMap &BlockingStoresDispSizeMap);

  bool alias(const MachineMemOperand &Op1, const MachineMemOperand &Op2) const;

  unsigned getRegSizeInBytes(MachineInstr *Inst);
};

class X86AvoidSFBLegacy : public MachineFunctionPass {
public:
  static char ID;
  X86AvoidSFBLegacy() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "X86 Avoid Store Forwarding Blocks";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    MachineFunctionPass::getAnalysisUsage(AU);
    AU.addRequired<AAResultsWrapperPass>();
  }
};

} // end anonymous namespace

char X86AvoidSFBLegacy::ID = 0;

INITIALIZE_PASS_BEGIN(X86AvoidSFBLegacy, DEBUG_TYPE, "Machine code sinking",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(X86AvoidSFBLegacy, DEBUG_TYPE, "Machine code sinking",
                    false, false)

FunctionPass *llvm::createX86AvoidStoreForwardingBlocksLegacyPass() {
  return new X86AvoidSFBLegacy();
}

static bool isXMMLoadOpcode(unsigned Opcode) {
  return Opcode == X86::MOVUPSrm || Opcode == X86::MOVAPSrm ||
         Opcode == X86::VMOVUPSrm || Opcode == X86::VMOVAPSrm ||
         Opcode == X86::VMOVUPDrm || Opcode == X86::VMOVAPDrm ||
         Opcode == X86::VMOVDQUrm || Opcode == X86::VMOVDQArm ||
         Opcode == X86::VMOVUPSZ128rm || Opcode == X86::VMOVAPSZ128rm ||
         Opcode == X86::VMOVUPDZ128rm || Opcode == X86::VMOVAPDZ128rm ||
         Opcode == X86::VMOVDQU64Z128rm || Opcode == X86::VMOVDQA64Z128rm ||
         Opcode == X86::VMOVDQU32Z128rm || Opcode == X86::VMOVDQA32Z128rm;
}
static bool isYMMLoadOpcode(unsigned Opcode) {
  return Opcode == X86::VMOVUPSYrm || Opcode == X86::VMOVAPSYrm ||
         Opcode == X86::VMOVUPDYrm || Opcode == X86::VMOVAPDYrm ||
         Opcode == X86::VMOVDQUYrm || Opcode == X86::VMOVDQAYrm ||
         Opcode == X86::VMOVUPSZ256rm || Opcode == X86::VMOVAPSZ256rm ||
         Opcode == X86::VMOVUPDZ256rm || Opcode == X86::VMOVAPDZ256rm ||
         Opcode == X86::VMOVDQU64Z256rm || Opcode == X86::VMOVDQA64Z256rm ||
         Opcode == X86::VMOVDQU32Z256rm || Opcode == X86::VMOVDQA32Z256rm;
}

static bool isPotentialBlockedMemCpyLd(unsigned Opcode) {
  return isXMMLoadOpcode(Opcode) || isYMMLoadOpcode(Opcode);
}

static bool isPotentialBlockedMemCpyPair(unsigned LdOpcode, unsigned StOpcode) {
  switch (LdOpcode) {
  case X86::MOVUPSrm:
  case X86::MOVAPSrm:
    return StOpcode == X86::MOVUPSmr || StOpcode == X86::MOVAPSmr;
  case X86::VMOVUPSrm:
  case X86::VMOVAPSrm:
    return StOpcode == X86::VMOVUPSmr || StOpcode == X86::VMOVAPSmr;
  case X86::VMOVUPDrm:
  case X86::VMOVAPDrm:
    return StOpcode == X86::VMOVUPDmr || StOpcode == X86::VMOVAPDmr;
  case X86::VMOVDQUrm:
  case X86::VMOVDQArm:
    return StOpcode == X86::VMOVDQUmr || StOpcode == X86::VMOVDQAmr;
  case X86::VMOVUPSZ128rm:
  case X86::VMOVAPSZ128rm:
    return StOpcode == X86::VMOVUPSZ128mr || StOpcode == X86::VMOVAPSZ128mr;
  case X86::VMOVUPDZ128rm:
  case X86::VMOVAPDZ128rm:
    return StOpcode == X86::VMOVUPDZ128mr || StOpcode == X86::VMOVAPDZ128mr;
  case X86::VMOVUPSYrm:
  case X86::VMOVAPSYrm:
    return StOpcode == X86::VMOVUPSYmr || StOpcode == X86::VMOVAPSYmr;
  case X86::VMOVUPDYrm:
  case X86::VMOVAPDYrm:
    return StOpcode == X86::VMOVUPDYmr || StOpcode == X86::VMOVAPDYmr;
  case X86::VMOVDQUYrm:
  case X86::VMOVDQAYrm:
    return StOpcode == X86::VMOVDQUYmr || StOpcode == X86::VMOVDQAYmr;
  case X86::VMOVUPSZ256rm:
  case X86::VMOVAPSZ256rm:
    return StOpcode == X86::VMOVUPSZ256mr || StOpcode == X86::VMOVAPSZ256mr;
  case X86::VMOVUPDZ256rm:
  case X86::VMOVAPDZ256rm:
    return StOpcode == X86::VMOVUPDZ256mr || StOpcode == X86::VMOVAPDZ256mr;
  case X86::VMOVDQU64Z128rm:
  case X86::VMOVDQA64Z128rm:
    return StOpcode == X86::VMOVDQU64Z128mr || StOpcode == X86::VMOVDQA64Z128mr;
  case X86::VMOVDQU32Z128rm:
  case X86::VMOVDQA32Z128rm:
    return StOpcode == X86::VMOVDQU32Z128mr || StOpcode == X86::VMOVDQA32Z128mr;
  case X86::VMOVDQU64Z256rm:
  case X86::VMOVDQA64Z256rm:
    return StOpcode == X86::VMOVDQU64Z256mr || StOpcode == X86::VMOVDQA64Z256mr;
  case X86::VMOVDQU32Z256rm:
  case X86::VMOVDQA32Z256rm:
    return StOpcode == X86::VMOVDQU32Z256mr || StOpcode == X86::VMOVDQA32Z256mr;
  default:
    return false;
  }
}

static bool isPotentialBlockingStoreInst(unsigned Opcode, unsigned LoadOpcode) {
  bool PBlock = false;
  PBlock |= Opcode == X86::MOV64mr || Opcode == X86::MOV64mi32 ||
            Opcode == X86::MOV32mr || Opcode == X86::MOV32mi ||
            Opcode == X86::MOV16mr || Opcode == X86::MOV16mi ||
            Opcode == X86::MOV8mr || Opcode == X86::MOV8mi;
  if (isYMMLoadOpcode(LoadOpcode))
    PBlock |= Opcode == X86::VMOVUPSmr || Opcode == X86::VMOVAPSmr ||
              Opcode == X86::VMOVUPDmr || Opcode == X86::VMOVAPDmr ||
              Opcode == X86::VMOVDQUmr || Opcode == X86::VMOVDQAmr ||
              Opcode == X86::VMOVUPSZ128mr || Opcode == X86::VMOVAPSZ128mr ||
              Opcode == X86::VMOVUPDZ128mr || Opcode == X86::VMOVAPDZ128mr ||
              Opcode == X86::VMOVDQU64Z128mr ||
              Opcode == X86::VMOVDQA64Z128mr ||
              Opcode == X86::VMOVDQU32Z128mr || Opcode == X86::VMOVDQA32Z128mr;
  return PBlock;
}

static const int MOV128SZ = 16;
static const int MOV64SZ = 8;
static const int MOV32SZ = 4;
static const int MOV16SZ = 2;
static const int MOV8SZ = 1;

static unsigned getYMMtoXMMLoadOpcode(unsigned LoadOpcode) {
  switch (LoadOpcode) {
  case X86::VMOVUPSYrm:
  case X86::VMOVAPSYrm:
    return X86::VMOVUPSrm;
  case X86::VMOVUPDYrm:
  case X86::VMOVAPDYrm:
    return X86::VMOVUPDrm;
  case X86::VMOVDQUYrm:
  case X86::VMOVDQAYrm:
    return X86::VMOVDQUrm;
  case X86::VMOVUPSZ256rm:
  case X86::VMOVAPSZ256rm:
    return X86::VMOVUPSZ128rm;
  case X86::VMOVUPDZ256rm:
  case X86::VMOVAPDZ256rm:
    return X86::VMOVUPDZ128rm;
  case X86::VMOVDQU64Z256rm:
  case X86::VMOVDQA64Z256rm:
    return X86::VMOVDQU64Z128rm;
  case X86::VMOVDQU32Z256rm:
  case X86::VMOVDQA32Z256rm:
    return X86::VMOVDQU32Z128rm;
  default:
    llvm_unreachable("Unexpected Load Instruction Opcode");
  }
  return 0;
}

static unsigned getYMMtoXMMStoreOpcode(unsigned StoreOpcode) {
  switch (StoreOpcode) {
  case X86::VMOVUPSYmr:
  case X86::VMOVAPSYmr:
    return X86::VMOVUPSmr;
  case X86::VMOVUPDYmr:
  case X86::VMOVAPDYmr:
    return X86::VMOVUPDmr;
  case X86::VMOVDQUYmr:
  case X86::VMOVDQAYmr:
    return X86::VMOVDQUmr;
  case X86::VMOVUPSZ256mr:
  case X86::VMOVAPSZ256mr:
    return X86::VMOVUPSZ128mr;
  case X86::VMOVUPDZ256mr:
  case X86::VMOVAPDZ256mr:
    return X86::VMOVUPDZ128mr;
  case X86::VMOVDQU64Z256mr:
  case X86::VMOVDQA64Z256mr:
    return X86::VMOVDQU64Z128mr;
  case X86::VMOVDQU32Z256mr:
  case X86::VMOVDQA32Z256mr:
    return X86::VMOVDQU32Z128mr;
  default:
    llvm_unreachable("Unexpected Load Instruction Opcode");
  }
  return 0;
}

static int getAddrOffset(const MachineInstr *MI) {
  const MCInstrDesc &Descl = MI->getDesc();
  int AddrOffset = X86II::getMemoryOperandNo(Descl.TSFlags);
  assert(AddrOffset != -1 && "Expected Memory Operand");
  AddrOffset += X86II::getOperandBias(Descl);
  return AddrOffset;
}

static MachineOperand &getBaseOperand(MachineInstr *MI) {
  int AddrOffset = getAddrOffset(MI);
  return MI->getOperand(AddrOffset + X86::AddrBaseReg);
}

static MachineOperand &getDispOperand(MachineInstr *MI) {
  int AddrOffset = getAddrOffset(MI);
  return MI->getOperand(AddrOffset + X86::AddrDisp);
}

// Relevant addressing modes contain only base register and immediate
// displacement or frameindex and immediate displacement.
// TODO: Consider expanding to other addressing modes in the future
static bool isRelevantAddressingMode(MachineInstr *MI) {
  int AddrOffset = getAddrOffset(MI);
  const MachineOperand &Base = getBaseOperand(MI);
  const MachineOperand &Disp = getDispOperand(MI);
  const MachineOperand &Scale = MI->getOperand(AddrOffset + X86::AddrScaleAmt);
  const MachineOperand &Index = MI->getOperand(AddrOffset + X86::AddrIndexReg);
  const MachineOperand &Segment = MI->getOperand(AddrOffset + X86::AddrSegmentReg);

  if (!((Base.isReg() && Base.getReg() != X86::NoRegister) || Base.isFI()))
    return false;
  if (!Disp.isImm())
    return false;
  if (Scale.getImm() != 1)
    return false;
  if (!(Index.isReg() && Index.getReg() == X86::NoRegister))
    return false;
  if (!(Segment.isReg() && Segment.getReg() == X86::NoRegister))
    return false;
  return true;
}

// Collect potentially blocking stores.
// Limit the number of instructions backwards we want to inspect
// since the effect of store block won't be visible if the store
// and load instructions have enough instructions in between to
// keep the core busy.
static SmallVector<MachineInstr *, 2>
findPotentialBlockers(MachineInstr *LoadInst) {
  SmallVector<MachineInstr *, 2> PotentialBlockers;
  unsigned BlockCount = 0;
  const unsigned InspectionLimit = X86AvoidSFBInspectionLimit;
  for (auto PBInst = std::next(MachineBasicBlock::reverse_iterator(LoadInst)),
            E = LoadInst->getParent()->rend();
       PBInst != E; ++PBInst) {
    if (PBInst->isMetaInstruction())
      continue;
    BlockCount++;
    if (BlockCount >= InspectionLimit)
      break;
    MachineInstr &MI = *PBInst;
    if (MI.getDesc().isCall())
      return PotentialBlockers;
    PotentialBlockers.push_back(&MI);
  }
  // If we didn't get to the instructions limit try predecessing blocks.
  // Ideally we should traverse the predecessor blocks in depth with some
  // coloring algorithm, but for now let's just look at the first order
  // predecessors.
  if (BlockCount < InspectionLimit) {
    MachineBasicBlock *MBB = LoadInst->getParent();
    int LimitLeft = InspectionLimit - BlockCount;
    for (MachineBasicBlock *PMBB : MBB->predecessors()) {
      int PredCount = 0;
      for (MachineInstr &PBInst : llvm::reverse(*PMBB)) {
        if (PBInst.isMetaInstruction())
          continue;
        PredCount++;
        if (PredCount >= LimitLeft)
          break;
        if (PBInst.getDesc().isCall())
          break;
        PotentialBlockers.push_back(&PBInst);
      }
    }
  }
  return PotentialBlockers;
}

void X86AvoidSFBImpl::emitCopy(const CopyPlan &Plan, const CopyPiece &Piece,
                               MachineInstr *&LastLoad,
                               MachineInstr *&LastStore) {
  MachineInstr *LoadInst = Plan.Load, *StoreInst = Plan.Store;
  auto [NLoadOpcode, NStoreOpcode, Size, Offset, ForwardedReg] = Piece;
  MachineOperand &LoadBase = getBaseOperand(LoadInst);
  MachineOperand &StoreBase = getBaseOperand(StoreInst);
  MachineBasicBlock *MBB = LoadInst->getParent();
  MachineMemOperand *LMMO = *LoadInst->memoperands_begin();
  MachineMemOperand *SMMO = *StoreInst->memoperands_begin();

  Register Reg1 = ForwardedReg;
  if (!Reg1) {
    Reg1 =
        MRI->createVirtualRegister(TII->getRegClass(TII->get(NLoadOpcode), 0));
    LastLoad = BuildMI(*MBB, LoadInst, LoadInst->getDebugLoc(),
                       TII->get(NLoadOpcode), Reg1)
                   .add(LoadBase)
                   .addImm(1)
                   .addReg(X86::NoRegister)
                   .addImm(getDispOperand(LoadInst).getImm() + Offset)
                   .addReg(X86::NoRegister)
                   .addMemOperand(MBB->getParent()->getMachineMemOperand(
                       LMMO, Offset, Size));
    if (LoadBase.isReg())
      getBaseOperand(LastLoad).setIsKill(false);
    LLVM_DEBUG(LastLoad->dump());
  } else {
    // The blocking store may have been the last use before forwarding.
    MRI->clearKillFlags(Reg1);
  }
  // For disjoint consecutive accesses, interleave the copies to reduce
  // register pressure.
  MachineInstr *StInst = StoreInst;
  auto PrevInstrIt = prev_nodbg(MachineBasicBlock::instr_iterator(StoreInst),
                                MBB->instr_begin());
  if (Plan.IsDisjoint && PrevInstrIt.getNodePtr() == LoadInst)
    StInst = LoadInst;
  MachineInstr *NewStore =
      BuildMI(*MBB, StInst, StInst->getDebugLoc(), TII->get(NStoreOpcode))
          .add(StoreBase)
          .addImm(1)
          .addReg(X86::NoRegister)
          .addImm(getDispOperand(StoreInst).getImm() + Offset)
          .addReg(X86::NoRegister)
          .addReg(Reg1)
          .addMemOperand(
              MBB->getParent()->getMachineMemOperand(SMMO, Offset, Size));
  if (StoreBase.isReg())
    getBaseOperand(NewStore).setIsKill(false);
  MachineOperand &StoreSrcVReg = StoreInst->getOperand(X86::AddrNumOperands);
  assert(StoreSrcVReg.isReg() && "Expected virtual register");
  NewStore->getOperand(X86::AddrNumOperands)
      .setIsKill(!ForwardedReg && StoreSrcVReg.isKill());
  LastStore = NewStore;
  LLVM_DEBUG(NewStore->dump());
}

bool X86AvoidSFBImpl::alias(const MachineMemOperand &Op1,
                            const MachineMemOperand &Op2) const {
  if (!Op1.getValue() || !Op2.getValue())
    return true;

  int64_t MinOffset = std::min(Op1.getOffset(), Op2.getOffset());
  int64_t Overlapa = Op1.getSize().getValue() + Op1.getOffset() - MinOffset;
  int64_t Overlapb = Op2.getSize().getValue() + Op2.getOffset() - MinOffset;

  return !AA->isNoAlias(
      MemoryLocation(Op1.getValue(), Overlapa, Op1.getAAInfo()),
      MemoryLocation(Op2.getValue(), Overlapb, Op2.getAAInfo()));
}

void X86AvoidSFBImpl::findPotentiallylBlockedCopies(MachineFunction &MF) {
  for (auto &MBB : MF)
    for (auto &MI : MBB) {
      if (!isPotentialBlockedMemCpyLd(MI.getOpcode()))
        continue;
      Register DefVR = MI.getOperand(0).getReg();
      if (!MRI->hasOneNonDBGUse(DefVR))
        continue;
      for (MachineOperand &StoreMO :
           llvm::make_early_inc_range(MRI->use_nodbg_operands(DefVR))) {
        MachineInstr &StoreMI = *StoreMO.getParent();
        // Splitting ordered accesses would change their observable accesses.
        if (StoreMI.getParent() == MI.getParent() &&
            isPotentialBlockedMemCpyPair(MI.getOpcode(), StoreMI.getOpcode()) &&
            isRelevantAddressingMode(&MI) &&
            isRelevantAddressingMode(&StoreMI) &&
            MI.hasOneMemOperand() && StoreMI.hasOneMemOperand()) {
          if ((*MI.memoperands_begin())->isUnordered() &&
              !(*MI.memoperands_begin())->isAtomic() &&
              (*StoreMI.memoperands_begin())->isUnordered() &&
              !(*StoreMI.memoperands_begin())->isAtomic())
            BlockedLoadsStoresPairs.push_back(std::make_pair(&MI, &StoreMI));
        }
      }
    }
}

unsigned X86AvoidSFBImpl::getRegSizeInBytes(MachineInstr *LoadInst) {
  const auto *TRC = TII->getRegClass(TII->get(LoadInst->getOpcode()), 0);
  return TRI->getRegSizeInBits(*TRC) / 8;
}

void X86AvoidSFBImpl::planCopies(
    CopyPlan &Plan, const DisplacementSizeMap &BlockingStoresDispSizeMap) {
  auto AddPieces = [&](unsigned Size, int64_t Offset) {
    static constexpr std::pair<unsigned, unsigned> Opcodes[] = {
        {X86::MOV8rm, X86::MOV8mr},
        {X86::MOV16rm, X86::MOV16mr},
        {X86::MOV32rm, X86::MOV32mr},
        {X86::MOV64rm, X86::MOV64mr}};
    while (Size) {
      unsigned LogSize = Log2_32(std::min(Size, unsigned(MOV64SZ)));
      auto [LoadOpcode, StoreOpcode] = Opcodes[LogSize];
      unsigned PieceSize = 1U << LogSize;
      if (Size >= MOV128SZ && isYMMLoadOpcode(Plan.Load->getOpcode())) {
        LoadOpcode = getYMMtoXMMLoadOpcode(Plan.Load->getOpcode());
        StoreOpcode = getYMMtoXMMStoreOpcode(Plan.Store->getOpcode());
        PieceSize = MOV128SZ;
      }
      Plan.Pieces.push_back({LoadOpcode, StoreOpcode, PieceSize, Offset});
      Size -= PieceSize;
      Offset += PieceSize;
    }
  };
  int64_t LoadDisp = getDispOperand(Plan.Load).getImm();
  int64_t Offset = 0;
  for (auto [Disp, Size] : BlockingStoresDispSizeMap) {
    int64_t Begin = Disp - LoadDisp;
    int64_t End = Begin + Size;
    // Do not copy an overlapping part of a blocker twice.
    Begin = std::max(Begin, Offset);
    AddPieces(Begin - Offset, Offset);
    AddPieces(End - Begin, Begin);
    Offset = End;
  }
  AddPieces(getRegSizeInBytes(Plan.Load) - Offset, Offset);
}

static bool hasSameBaseOpValue(MachineInstr *LoadInst,
                               MachineInstr *StoreInst) {
  const MachineOperand &LoadBase = getBaseOperand(LoadInst);
  const MachineOperand &StoreBase = getBaseOperand(StoreInst);
  if (LoadBase.isReg() != StoreBase.isReg())
    return false;
  if (LoadBase.isReg())
    return LoadBase.getReg() == StoreBase.getReg();
  return LoadBase.getIndex() == StoreBase.getIndex();
}

static bool isBlockingStore(int64_t LoadDispImm, unsigned LoadSize,
                            int64_t StoreDispImm, unsigned StoreSize) {
  return ((StoreDispImm >= LoadDispImm) &&
          (StoreDispImm <= LoadDispImm + (LoadSize - StoreSize)));
}

// Keep track of all stores blocking a load
static void
updateBlockingStoresDispSizeMap(DisplacementSizeMap &BlockingStoresDispSizeMap,
                                int64_t DispImm, unsigned Size) {
  auto [It, Inserted] = BlockingStoresDispSizeMap.try_emplace(DispImm, Size);
  // Choose the smallest blocking store starting at this displacement.
  if (!Inserted && It->second > Size)
    It->second = Size;
}

// Remove blocking stores contained in each other.
static void
removeRedundantBlockingStores(DisplacementSizeMap &BlockingStoresDispSizeMap) {
  if (BlockingStoresDispSizeMap.size() <= 1)
    return;

  SmallVector<std::pair<int64_t, unsigned>, 0> DispSizeStack;
  for (auto DispSizePair : BlockingStoresDispSizeMap) {
    int64_t CurrDisp = DispSizePair.first;
    unsigned CurrSize = DispSizePair.second;
    while (DispSizeStack.size()) {
      int64_t PrevDisp = DispSizeStack.back().first;
      unsigned PrevSize = DispSizeStack.back().second;
      if (CurrDisp + CurrSize > PrevDisp + PrevSize)
        break;
      DispSizeStack.pop_back();
    }
    DispSizeStack.push_back(DispSizePair);
  }
  BlockingStoresDispSizeMap.clear();
  for (auto Disp : DispSizeStack)
    BlockingStoresDispSizeMap.insert(Disp);
}

// Seed the existing pressure tracker with live-outs, including PHI edge uses,
// then walk back to the adjacent copy. Compute liveness only if the new path
// needs it; disjoint copies retain their existing analysis requirements.
bool X86AvoidSFBImpl::selectCopyRegisters(CopyPlan &Plan) {
  MachineBasicBlock &MBB = *Plan.Load->getParent();
  MachineFunction &MF = *MBB.getParent();
  if (!LV) {
    LV = std::make_unique<LiveVariables>(MF);
    RCI.runOnMachineFunction(MF);
  }
  SmallVector<VRegMaskOrUnit, 16> LiveRegs;
  auto AddReg = [&](Register Reg) {
    if (Reg.isVirtual())
      LiveRegs.emplace_back(VirtRegOrUnit(Reg), LaneBitmask::getAll());
    else if (Reg && MRI->isAllocatable(Reg))
      for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg()))
        LiveRegs.emplace_back(VirtRegOrUnit(Unit), LaneBitmask::getAll());
  };
  if (PressureMBB != &MBB) {
    PressureMBB = &MBB;
    // AliveBlocks includes PHI edge uses but excludes the defining block.
    // A value defined here is live out exactly when it has no local kill
    // (a dead definition is itself recorded as a kill).
    for (unsigned I = 0; I != MRI->getNumVirtRegs(); ++I) {
      Register Reg = Register::index2VirtReg(I);
      if (MachineInstr *Def = MRI->getVRegDef(Reg)) {
        LiveVariables::VarInfo &VI = LV->getVarInfo(Reg);
        if (VI.AliveBlocks.test(MBB.getNumber()) ||
            (Def->getParent() == &MBB && !VI.findKill(&MBB)))
          AddReg(Reg);
      }
    }
    LivePhysRegs PhysRegs(*TRI);
    PhysRegs.addLiveOutsNoPristines(MBB);
    for (MCPhysReg Reg : PhysRegs)
      AddReg(Reg);

    Tracker.init(&MF, &RCI, nullptr, &MBB, MBB.end(), false, false);
    Tracker.addLiveRegs(LiveRegs);
  }
  do {
    Tracker.recede();
  } while (&*Tracker.getPos() != Plan.Store);
  // The source address must survive all split loads even if the original
  // vector load killed it. Full-register masks conservatively include subregs.
  LiveRegs.clear();
  if (getBaseOperand(Plan.Load).isReg())
    AddReg(getBaseOperand(Plan.Load).getReg());
  // Forwarding extends the stored value to the copy. Let the tracker account
  // for it once, including when it was already live through the copy.
  for (const CopyPiece &Piece : Plan.Pieces)
    if (Piece.ForwardedReg)
      AddReg(Piece.ForwardedReg);
  Tracker.addLiveRegs(LiveRegs);
  const auto OriginalPressure = Tracker.getRegSetPressureAtPos();
  auto SetPressure = OriginalPressure;
  for (auto PS = MRI->getPressureSets(
           VirtRegOrUnit(Plan.Load->getOperand(0).getReg()));
       PS.isValid(); ++PS)
    SetPressure[*PS] -= PS.getWeight();

  auto TryRegisterClass = [&](unsigned Opcode) {
    const TargetRegisterClass *RC = TII->getRegClass(TII->get(Opcode), 0);
    unsigned Weight = TRI->getRegClassWeight(RC).RegWeight;
    unsigned Available = llvm::count_if(RCI.getOrder(RC), [&](MCPhysReg Reg) {
      return !RCI.getLastCalleeSavedAlias(Reg);
    });
    for (const int *PS = TRI->getRegClassPressureSets(RC); *PS != -1; ++PS) {
      unsigned Limit =
          std::min(RCI.getRegPressureSetLimit(*PS), Available * Weight);
      // Leave headroom when adding pressure, but allow replacing a register
      // already needed by the original copy in a full pressure set.
      if (SetPressure[*PS] + Weight > OriginalPressure[*PS] &&
          SetPressure[*PS] + Weight >= Limit)
        return false;
    }
    for (const int *PS = TRI->getRegClassPressureSets(RC); *PS != -1; ++PS)
      SetPressure[*PS] += Weight;
    return true;
  };
  const X86Subtarget &ST = MF.getSubtarget<X86Subtarget>();
  for (CopyPiece &Piece : Plan.Pieces) {
    if (Piece.ForwardedReg || TryRegisterClass(Piece.LoadOpcode))
      continue;
    // A qword can also travel through an XMM register without a shuffle or a
    // register-bank transfer. Keep the GPR form when it has headroom.
    unsigned LoadOpcode = ST.hasAVX() ? X86::VMOVQI2PQIrm : X86::MOVQI2PQIrm;
    if (Piece.LoadOpcode != X86::MOV64rm || !ST.hasSSE2() ||
        !TryRegisterClass(LoadOpcode))
      return false;
    Piece.LoadOpcode = LoadOpcode;
    Piece.StoreOpcode = ST.hasAVX() ? X86::VMOVPQI2QImr : X86::MOVPQI2QImr;
  }
  return true;
}

bool X86AvoidSFBImpl::runOnMachineFunction(MachineFunction &MF) {
  if (DisableX86AvoidStoreForwardBlocks ||
      !MF.getSubtarget<X86Subtarget>().is64Bit())
    return false;

  MRI = &MF.getRegInfo();
  assert(MRI->isSSA() && "Expected MIR to be in SSA form");
  TII = MF.getSubtarget<X86Subtarget>().getInstrInfo();
  TRI = MF.getSubtarget<X86Subtarget>().getRegisterInfo();
  SmallVector<CopyPlan, 2> Plans;
  LLVM_DEBUG(dbgs() << "Start X86AvoidStoreForwardBlocks\n";);
  // Look for a load then a store to XMM/YMM which look like a memcpy
  findPotentiallylBlockedCopies(MF);

  // Analyze candidates backwards so pressure tracking scans each block once.
  // Retain only profitable plans; do not change instructions until all queries
  // against the original liveness information are complete.
  for (auto [LoadInst, StoreInst] : llvm::reverse(BlockedLoadsStoresPairs)) {
    bool IsDisjoint = !alias(**LoadInst->memoperands_begin(),
                             **StoreInst->memoperands_begin());
    bool IsAdjacent = next_nodbg(MachineBasicBlock::instr_iterator(LoadInst),
                                 LoadInst->getParent()->instr_end())
                          .getNodePtr() == StoreInst;
    // Newly admitted copies must have a short live range and a guaranteed
    // blocker. Leave the existing disjoint-copy policy unchanged.
    if (!IsDisjoint && (MF.getFunction().hasOptSize() || !IsAdjacent))
      continue;
    int64_t LdDispImm = getDispOperand(LoadInst).getImm();
    DisplacementSizeMap BlockingStoresDispSizeMap;
    MachineInstr *ImmediateBlocker = nullptr;

    SmallVector<MachineInstr *, 2> PotentialBlockers =
        findPotentialBlockers(LoadInst);
    for (auto *PBInst : PotentialBlockers) {
      if (!IsDisjoint && PBInst->getParent() != LoadInst->getParent())
        continue;
      if (!isPotentialBlockingStoreInst(PBInst->getOpcode(),
                                        LoadInst->getOpcode()) ||
          !isRelevantAddressingMode(PBInst) || !PBInst->hasOneMemOperand())
        continue;
      int64_t PBstDispImm = getDispOperand(PBInst).getImm();
      unsigned PBstSize = (*PBInst->memoperands_begin())->getSize().getValue();
      // This check doesn't cover all cases, but it will suffice for now.
      // TODO: take branch probability into consideration, if the blocking
      // store is in an unreached block, breaking the memcopy could lose
      // performance.
      if (hasSameBaseOpValue(LoadInst, PBInst) &&
          isBlockingStore(LdDispImm, getRegSizeInBytes(LoadInst), PBstDispImm,
                          PBstSize)) {
        if (prev_nodbg(MachineBasicBlock::instr_iterator(LoadInst),
                       LoadInst->getParent()->instr_begin())
                .getNodePtr() == PBInst)
          ImmediateBlocker = PBInst;
        updateBlockingStoresDispSizeMap(BlockingStoresDispSizeMap, PBstDispImm,
                                        PBstSize);
      }
    }

    if (BlockingStoresDispSizeMap.empty() || (!IsDisjoint && !ImmediateBlocker))
      continue;

    removeRedundantBlockingStores(BlockingStoresDispSizeMap);
    CopyPlan Plan{LoadInst, StoreInst, IsDisjoint, {}};
    planCopies(Plan, BlockingStoresDispSizeMap);
    // Keep the value of an adjacent store, not just its displacement and size.
    // Requiring an adjacent copy also avoids extending it across instructions
    // that are not covered by the copy's pressure check.
    if (ImmediateBlocker && IsAdjacent &&
        (*ImmediateBlocker->memoperands_begin())->isUnordered() &&
        !(*ImmediateBlocker->memoperands_begin())->isAtomic()) {
      const MachineOperand &Src =
          ImmediateBlocker->getOperand(X86::AddrNumOperands);
      if (Src.isReg() && Src.getReg().isVirtual() && !Src.getSubReg() &&
          !Src.isUndef())
        for (CopyPiece &Piece : Plan.Pieces)
          if (Piece.StoreOpcode == ImmediateBlocker->getOpcode() &&
              getDispOperand(ImmediateBlocker).getImm() ==
                  LdDispImm + Piece.Offset)
            Piece.ForwardedReg = Src.getReg();
    }
    bool HasForwardedReg =
        llvm::any_of(Plan.Pieces, [](const CopyPiece &Piece) {
          return bool(Piece.ForwardedReg);
        });
    if ((!IsDisjoint || HasForwardedReg) && !selectCopyRegisters(Plan)) {
      if (!IsDisjoint)
        continue;
      // Preserve the existing disjoint split without extending a live range.
      Plan.Pieces.clear();
      planCopies(Plan, BlockingStoresDispSizeMap);
    }
    Plans.push_back(std::move(Plan));
  }

  // Emit in program order, preserving the order of virtual register creation.
  for (const CopyPlan &Plan : llvm::reverse(Plans)) {
    LLVM_DEBUG(dbgs() << "Blocked load and store instructions:\n";
               Plan.Load->dump(); Plan.Store->dump();
               dbgs() << "Replaced with:\n");
    MachineInstr *LastLoad = nullptr, *LastStore = nullptr;
    for (const CopyPiece &Piece : Plan.Pieces)
      emitCopy(Plan, Piece, LastLoad, LastStore);
    if (LastLoad && getBaseOperand(Plan.Load).isReg())
      getBaseOperand(LastLoad).setIsKill(getBaseOperand(Plan.Load).isKill());
    if (getBaseOperand(Plan.Store).isReg())
      getBaseOperand(LastStore).setIsKill(getBaseOperand(Plan.Store).isKill());
    Plan.Load->eraseFromParent();
    Plan.Store->eraseFromParent();
  }
  BlockedLoadsStoresPairs.clear();
  LLVM_DEBUG(dbgs() << "End X86AvoidStoreForwardBlocks\n";);

  return !Plans.empty();
}

bool X86AvoidSFBLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  AliasAnalysis *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  X86AvoidSFBImpl Impl(AA);
  return Impl.runOnMachineFunction(MF);
}

PreservedAnalyses
X86AvoidStoreForwardingBlocksPass::run(MachineFunction &MF,
                                       MachineFunctionAnalysisManager &MFAM) {
  AliasAnalysis *AA =
      &MFAM.getResult<FunctionAnalysisManagerMachineFunctionProxy>(MF)
           .getManager()
           .getResult<AAManager>(MF.getFunction());
  X86AvoidSFBImpl Impl(AA);
  bool Changed = Impl.runOnMachineFunction(MF);
  return Changed ? getMachineFunctionPassPreservedAnalyses()
                 : PreservedAnalyses::all();
}
