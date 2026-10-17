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

TEST(CaptureTracking, GlobalAddressInFunction) {
  StringRef Assembly = R"(
    @g = constant [4 x i32] [i32 1, i32 2, i32 3, i32 4]
    @h = external global i32
    @holder = global ptr @g
    declare void @reads(ptr captures(none) readonly)
    declare void @keeps(ptr)

    define i32 @loop() {
    entry:
      br label %loop
    loop:
      %p = phi ptr [ @g, %entry ], [ %n, %loop ]
      %v = load i32, ptr %p
      %n = getelementptr inbounds i32, ptr %p, i64 1
      %d = icmp eq ptr %n, getelementptr inbounds (i8, ptr @g, i64 16)
      br i1 %d, label %exit, label %loop
    exit:
      ret i32 %v
    }

    define void @passes() {
      call void @reads(ptr getelementptr inbounds (i8, ptr @g, i64 4))
      ret void
    }

    define i1 @compares(ptr %q) {
      %c = icmp eq ptr %q, @g
      ret i1 %c
    }

    define void @keeps_it() {
      call void @keeps(ptr @g)
      ret void
    }

    define ptr @returns() {
      ret ptr @g
    }

    define i32 @in_aggregate() {
      %s = extractvalue { ptr, i32 } { ptr @g, i32 0 }, 0
      %v = load i32, ptr %s
      ret i32 %v
    }

    define void @other() {
      call void @keeps(ptr @g)
      ret void
    }
  )";

  LLVMContext Context;
  SMDiagnostic Error;
  auto M = parseAssemblyString(Assembly, Error, Context);
  ASSERT_TRUE(M) << "Bad assembly?";
  const GlobalValue *G = M->getNamedValue("g");
  auto Captures = [&](const char *FName) {
    return globalAddressMayBeCapturedIn(G, *M->getFunction(FName));
  };
  // A function's own uses decide, whatever other functions and the
  // initializers of other globals do with the address.
  EXPECT_FALSE(Captures("loop"));
  EXPECT_FALSE(Captures("passes"));
  EXPECT_TRUE(Captures("compares"));
  EXPECT_TRUE(Captures("keeps_it"));
  EXPECT_TRUE(Captures("returns"));
  EXPECT_TRUE(Captures("in_aggregate"));
}
