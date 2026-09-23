//===- ScheduleDAGInstrsTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {
#include "MFCommon.inc"

class MemorySubtarget : public BogusSubtarget {
public:
  using BogusSubtarget::BogusSubtarget;
  mutable unsigned Calls = 0;
  unsigned ExpectedLatency = 0;

  void adjustSchedDependency(SUnit *Def, int DefOpIdx, SUnit *Use, int UseOpIdx,
                             SDep &Dep,
                             const TargetSchedModel *Model) const override {
    ++Calls;
    EXPECT_EQ(DefOpIdx, -1);
    EXPECT_EQ(UseOpIdx, -1);
    EXPECT_TRUE(Dep.isNormalMemory());
    EXPECT_EQ(Dep.getSUnit(), Def);
    EXPECT_NE(Def, Use);
    EXPECT_NE(Model, nullptr);
    EXPECT_EQ(Dep.getLatency(), ExpectedLatency);
    Dep.setLatency(7);
  }
};

class MemoryDAG : public ScheduleDAGInstrs {
public:
  explicit MemoryDAG(MachineFunction &MF)
      : ScheduleDAGInstrs(MF, nullptr, false) {}
  using ScheduleDAGInstrs::addChainDependency;
  void schedule() override {}
};

class MemoryDependencyTest : public testing::Test {
protected:
  LLVMContext Context;
  Module M{"test", Context};
  BogusTargetMachine TM;
  MemorySubtarget ST{TM};
  MachineModuleInfo MMI{&TM};
  std::unique_ptr<MachineFunction> MF;
  MachineBasicBlock *MBB;
  GlobalVariable *Memory;
  const MCInstrDesc LoadDesc = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 1ULL << MCID::MayLoad, 0};
  const MCInstrDesc StoreDesc = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 1ULL << MCID::MayStore, 0};

  void SetUp() override {
    auto *F =
        Function::Create(FunctionType::get(Type::getVoidTy(Context), false),
                         GlobalValue::ExternalLinkage, "test", M);
    MF = std::make_unique<MachineFunction>(*F, TM, ST, MMI.getContext(), 0);
    MBB = MF->CreateMachineBasicBlock();
    MF->push_back(MBB);
    Memory = new GlobalVariable(M, ArrayType::get(Type::getInt64Ty(Context), 2),
                                false, GlobalValue::ExternalLinkage, nullptr,
                                "memory");
  }

  MachineInstr *memoryInstr(bool Store, int64_t Offset) {
    auto *MI = MF->CreateMachineInstr(Store ? StoreDesc : LoadDesc, DebugLoc());
    MBB->push_back(MI);
    MI->addMemOperand(
        *MF, MF->getMachineMemOperand(MachinePointerInfo(Memory, Offset),
                                      Store ? MachineMemOperand::MOStore
                                            : MachineMemOperand::MOLoad,
                                      8, Align(8)));
    return MI;
  }
};

TEST_F(MemoryDependencyTest, AdjustMemoryLatency) {
  // Exercise true, output and anti dependencies. The adjustment must update
  // both ends of the edge without weakening its ordering.
  for (auto [StoreA, StoreB] :
       {std::pair{true, false}, {true, true}, {false, true}}) {
    MemoryDAG DAG(*MF);
    SUnit A(memoryInstr(StoreA, 0), 0), B(memoryInstr(StoreB, 0), 1);
    ST.ExpectedLatency = StoreA && !StoreB ? 1 : 0;
    DAG.addChainDependency(&A, &B, ST.ExpectedLatency);
    ASSERT_EQ(B.Preds.size(), 1u);
    ASSERT_EQ(A.Succs.size(), 1u);
    EXPECT_EQ(B.Preds[0].getSUnit(), &A);
    EXPECT_EQ(A.Succs[0].getSUnit(), &B);
    EXPECT_EQ(B.Preds[0].getLatency(), 7u);
    EXPECT_EQ(A.Succs[0].getLatency(), 7u);
    EXPECT_TRUE(B.Preds[0].isNormalMemory());
    EXPECT_FALSE(B.Preds[0].isWeak());
  }
  EXPECT_EQ(ST.Calls, 3u);
}

TEST_F(MemoryDependencyTest, IndependentAccesses) {
  MemoryDAG DAG(*MF);
  SUnit Store(memoryInstr(true, 0), 0), Load(memoryInstr(false, 8), 1);
  DAG.addChainDependency(&Store, &Load, 1);
  EXPECT_TRUE(Store.Succs.empty());
  EXPECT_TRUE(Load.Preds.empty());

  SUnit OtherLoad(memoryInstr(false, 8), 2);
  DAG.addChainDependency(&Load, &OtherLoad, 1);
  EXPECT_TRUE(Load.Succs.empty());
  EXPECT_TRUE(OtherLoad.Preds.empty());
  EXPECT_EQ(ST.Calls, 0u);
}

TEST_F(MemoryDependencyTest, DefaultAdjustment) {
  MF = createMachineFunction(Context, M, "default");
  MBB = MF->CreateMachineBasicBlock();
  MF->push_back(MBB);
  MemoryDAG DAG(*MF);
  SUnit Store(memoryInstr(true, 0), 0), Load(memoryInstr(false, 0), 1);
  DAG.addChainDependency(&Store, &Load, 1);
  ASSERT_EQ(Load.Preds.size(), 1u);
  ASSERT_EQ(Store.Succs.size(), 1u);
  EXPECT_EQ(Load.Preds[0].getLatency(), 1u);
  EXPECT_EQ(Store.Succs[0].getLatency(), 1u);
}

TEST_F(MemoryDependencyTest, UnknownAccesses) {
  MemoryDAG DAG(*MF);
  auto *StoreMI = memoryInstr(true, 0);
  StoreMI->dropMemRefs(*MF);
  SUnit Store(StoreMI, 0), Load(memoryInstr(false, 8), 1);
  ST.ExpectedLatency = 1;
  DAG.addChainDependency(&Store, &Load, 1);
  ASSERT_EQ(Load.Preds.size(), 1u);
  EXPECT_TRUE(Load.Preds[0].isNormalMemory());
  EXPECT_EQ(Load.Preds[0].getLatency(), 7u);
  EXPECT_EQ(ST.Calls, 1u);
}
} // namespace
