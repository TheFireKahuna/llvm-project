//===- BasicTTIImplTest.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class MemcpyTTI : public BasicTTIImplBase<MemcpyTTI> {
  const Instruction *ExpectedInst;

public:
  MemcpyTTI(const DataLayout &DL, const Instruction *ExpectedInst)
      : BasicTTIImplBase(nullptr, DL), ExpectedInst(ExpectedInst) {}

  // Dispatch to the memcpy cost hook does not require target lowering.
  const TargetSubtargetInfo *getST() const {
    llvm_unreachable("Unexpected subtarget query");
  }
  const TargetLowering *getTLI() const {
    llvm_unreachable("Unexpected target lowering query");
  }

  InstructionCost
  getMemcpyCost(const Instruction *I,
                TargetTransformInfo::TargetCostKind Kind) const override {
    EXPECT_EQ(I, ExpectedInst);
    return 10 + static_cast<unsigned>(Kind);
  }
};

TEST(BasicTTIImplTest, MemcpyCostKind) {
  LLVMContext Context;
  SMDiagnostic Err;
  auto M = parseAssemblyString(R"(
    define void @copy(ptr %dst, ptr %src) {
      call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 16, i1 false)
      ret void
    }
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
  )",
                               Err, Context);
  ASSERT_TRUE(M);
  Function &F = *M->getFunction("copy");
  auto *Copy = cast<IntrinsicInst>(&F.getEntryBlock().front());
  TargetTransformInfo TTI(
      std::make_unique<MemcpyTTI>(M->getDataLayout(), Copy));
  TargetTransformInfo DefaultTTI(M->getDataLayout());
  IntrinsicCostAttributes ICA(Intrinsic::memcpy, *Copy);

  // Both public queries and the instruction-to-intrinsic dispatch must retain
  // the requested cost kind, even when the default model ignores it.
  for (auto Kind :
       {TargetTransformInfo::TCK_RecipThroughput,
        TargetTransformInfo::TCK_Latency, TargetTransformInfo::TCK_CodeSize,
        TargetTransformInfo::TCK_SizeAndLatency}) {
    InstructionCost Expected = 10 + static_cast<unsigned>(Kind);
    EXPECT_EQ(TTI.getMemcpyCost(Copy, Kind), Expected);
    EXPECT_EQ(TTI.getIntrinsicInstrCost(ICA, Kind), Expected);
    EXPECT_EQ(TTI.getInstructionCost(Copy, Kind), Expected);
    EXPECT_EQ(DefaultTTI.getMemcpyCost(Copy, Kind),
              TargetTransformInfo::TCC_Expensive);
  }
}
} // namespace
