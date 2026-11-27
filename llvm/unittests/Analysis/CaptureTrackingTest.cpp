//=======- CaptureTrackingTest.cpp - Unit test for the Capture Tracking ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

using namespace llvm;

TEST(CaptureTracking, MaxUsesToExplore) {
  StringRef Assembly = R"(
    ; Function Attrs: nounwind ssp uwtable
    declare void @doesnt_capture(ptr nocapture, ptr nocapture, ptr nocapture,
                                 ptr nocapture, ptr nocapture)

    ; %arg has 5 uses
    define void @test_few_uses(ptr %arg) {
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      ret void
    }

    ; %arg has 50 uses
    define void @test_many_uses(ptr %arg) {
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      call void @doesnt_capture(ptr %arg, ptr %arg, ptr %arg, ptr %arg, ptr %arg)
      ret void
    }
  )";

  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(Assembly, Error, Context);
  ASSERT_TRUE(M) << "Bad assembly?";

  auto Test = [&M](const char *FName, unsigned FalseMaxUsesLimit,
                   unsigned TrueMaxUsesLimit) {
    Function *F = M->getFunction(FName);
    ASSERT_NE(F, nullptr);
    Value *Arg = &*F->arg_begin();
    ASSERT_NE(Arg, nullptr);
    ASSERT_FALSE(PointerMayBeCaptured(Arg, true, FalseMaxUsesLimit));
    ASSERT_TRUE(PointerMayBeCaptured(Arg, true, TrueMaxUsesLimit));

    BasicBlock *EntryBB = &F->getEntryBlock();
    DominatorTree DT(*F);

    Instruction *Ret = EntryBB->getTerminator();
    ASSERT_TRUE(isa<ReturnInst>(Ret));
    ASSERT_FALSE(PointerMayBeCapturedBefore(Arg, true, Ret, &DT, false,
                                            FalseMaxUsesLimit));
    ASSERT_TRUE(PointerMayBeCapturedBefore(Arg, true, Ret, &DT, false,
                                           TrueMaxUsesLimit));
  };

  Test("test_few_uses", 6, 4);
  Test("test_many_uses", 50, 30);
}

struct CollectingCaptureTracker : public CaptureTracker {
  SmallVector<const Use *, 4> Captures;
  void tooManyUses() override { }
  Action captured(const Use *U, UseCaptureInfo CI) override {
    Captures.push_back(U);
    return Continue;
  }
};

TEST(CaptureTracking, MultipleUsesInSameInstruction) {
  StringRef Assembly = R"(
    declare void @call(ptr, ptr, ptr)

    define void @test(ptr %arg, ptr %ptr) {
      call void @call(ptr %arg, ptr nocapture %arg, ptr %arg) [ "bundle"(ptr %arg) ]
      cmpxchg ptr %ptr, ptr %arg, ptr %arg acq_rel monotonic
      icmp eq ptr %arg, %arg
      ret void
    }
  )";

  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(Assembly, Error, Context);
  ASSERT_TRUE(M) << "Bad assembly?";

  Function *F = M->getFunction("test");
  Value *Arg = &*F->arg_begin();
  BasicBlock *BB = &F->getEntryBlock();
  Instruction *Call = &*BB->begin();
  Instruction *CmpXChg = Call->getNextNode();
  Instruction *ICmp = CmpXChg->getNextNode();

  CollectingCaptureTracker CT;
  PointerMayBeCaptured(Arg, &CT);
  EXPECT_EQ(7u, CT.Captures.size());
  // Call arg 1
  EXPECT_EQ(Call, CT.Captures[0]->getUser());
  EXPECT_EQ(0u, CT.Captures[0]->getOperandNo());
  // Call arg 3
  EXPECT_EQ(Call, CT.Captures[1]->getUser());
  EXPECT_EQ(2u, CT.Captures[1]->getOperandNo());
  // Operand bundle arg
  EXPECT_EQ(Call, CT.Captures[2]->getUser());
  EXPECT_EQ(3u, CT.Captures[2]->getOperandNo());
  // Cmpxchg compare operand
  EXPECT_EQ(CmpXChg, CT.Captures[3]->getUser());
  EXPECT_EQ(1u, CT.Captures[3]->getOperandNo());
  // Cmpxchg new value operand
  EXPECT_EQ(CmpXChg, CT.Captures[4]->getUser());
  EXPECT_EQ(2u, CT.Captures[4]->getOperandNo());
  // ICmp first operand
  EXPECT_EQ(ICmp, CT.Captures[5]->getUser());
  EXPECT_EQ(0u, CT.Captures[5]->getOperandNo());
  // ICmp second operand
  EXPECT_EQ(ICmp, CT.Captures[6]->getUser());
  EXPECT_EQ(1u, CT.Captures[6]->getOperandNo());
}

TEST(CaptureTracking, GlobalAddressMayBeCapturedIn) {
  StringRef Assembly = R"(
    @g = constant [4 x i32] zeroinitializer
    @alias = alias [4 x i32], ptr @g
    @holder = constant ptr @g

    declare void @lent(ptr captures(none))
    declare void @kept(ptr)

    define i32 @reads(i64 %i) {
      call void @lent(ptr @g)
      %p = getelementptr i32, ptr @g, i64 %i
      %inside = icmp ult ptr %p, getelementptr (i32, ptr @g, i64 4)
      %held = load ptr, ptr @holder
      %v = load atomic i32, ptr %p seq_cst, align 4
      ret i32 %v
    }

    define void @keeps() {
      call void @kept(ptr getelementptr (i32, ptr @g, i64 1))
      ret void
    }

    define i1 @compares(ptr %p) {
      %c = icmp eq ptr %p, @g
      ret i1 %c
    }

    define i1 @aliases() {
      %c = icmp eq ptr @alias, @g
      ret i1 %c
    }

    define i64 @converts() {
      ret i64 ptrtoint (ptr @g to i64)
    }

    define i32 @observes() {
      %v = load volatile i32, ptr @g
      ret i32 %v
    }
  )";

  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(Assembly, Error, Context);
  ASSERT_TRUE(M) << "Bad assembly?";

  const GlobalValue *G = M->getNamedValue("g");
  EXPECT_FALSE(globalAddressMayBeCapturedIn(G, *M->getFunction("reads")));
  for (const char *Name :
       {"keeps", "compares", "aliases", "converts", "observes"})
    EXPECT_TRUE(globalAddressMayBeCapturedIn(G, *M->getFunction(Name))) << Name;
}
