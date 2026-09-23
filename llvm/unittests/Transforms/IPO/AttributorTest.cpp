//===- AttributorTest.cpp - Attributor unit tests ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/Attributor.h"
#include "AttributorTestBase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/PHITransAddr.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Transforms/Utils/CallGraphUpdater.h"
#include "gtest/gtest.h"

namespace llvm {

TEST_F(AttributorTestBase, IRPPositionCallBaseContext) {
  const char *ModuleString = R"(
    define i32 @foo(i32 %a) {
    entry:
      ret i32 %a
    }
  )";

  parseModule(ModuleString);

  Function *F = M->getFunction("foo");
  IRPosition Pos =
      IRPosition::function(*F, (const llvm::CallBase *)(uintptr_t)0xDEADBEEF);
  EXPECT_TRUE(Pos.hasCallBaseContext());
  EXPECT_FALSE(Pos.stripCallBaseContext().hasCallBaseContext());
}

TEST_F(AttributorTestBase, TestCast) {
  const char *ModuleString = R"(
    define i32 @foo(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      ret i32 %c
    }
  )";

  Module &M = parseModule(ModuleString);

  SetVector<Function *> Functions;
  AnalysisGetter AG;
  for (Function &F : M)
    Functions.insert(&F);

  CallGraphUpdater CGUpdater;
  BumpPtrAllocator Allocator;
  InformationCache InfoCache(M, AG, Allocator, nullptr);
  AttributorConfig AC(CGUpdater);
  Attributor A(Functions, InfoCache, AC);

  Function *F = M.getFunction("foo");

  const AbstractAttribute *AA =
      A.getOrCreateAAFor<AAIsDead>(IRPosition::function(*F));

  EXPECT_TRUE(AA);

  const auto *SFail = dyn_cast<AAAlign>(AA);
  const auto *SSucc = dyn_cast<AAIsDead>(AA);

  ASSERT_EQ(SFail, nullptr);
  ASSERT_TRUE(SSucc);
}

TEST_F(AttributorTestBase, AAReachabilityTest) {
  const char *ModuleString = R"(
    @x = external global i32
    define void @func4() {
      store i32 0, ptr @x
      ret void
    }

    define internal void @func3() {
      store i32 0, ptr @x
      ret void
    }

    define internal void @func8() {
      store i32 0, ptr @x
      ret void
    }

    define internal void @func2() {
    entry:
      call void @func3()
      ret void
    }

    define void @func1() {
    entry:
      call void @func2()
      ret void
    }

    declare void @unknown()
    define internal void @func5(ptr %ptr) {
    entry:
      call void %ptr()
      call void @unknown()
      ret void
    }

    define void @func6() {
    entry:
      store i32 0, ptr @x
      call void @func5(ptr @func3)
      ret void
    }

    define void @func7() {
    entry:
      call void @func2()
      call void @func4()
      ret void
    }

    define internal void @func9() {
    entry:
      call void @func2()
      call void @func8()
      ret void
    }

    define void @func10() {
    entry:
      call void @func9()
      call void @func4()
      ret void
    }

  )";

  Module &M = parseModule(ModuleString);

  SetVector<Function *> Functions;
  AnalysisGetter AG;
  for (Function &F : M)
    Functions.insert(&F);

  CallGraphUpdater CGUpdater;
  BumpPtrAllocator Allocator;
  InformationCache InfoCache(M, AG, Allocator, nullptr);
  AttributorConfig AC(CGUpdater);
  AC.DeleteFns = false;
  Attributor A(Functions, InfoCache, AC);

  Function &F1 = *M.getFunction("func1");
  Function &F3 = *M.getFunction("func3");
  Function &F4 = *M.getFunction("func4");
  Function &F6 = *M.getFunction("func6");
  Function &F7 = *M.getFunction("func7");
  Function &F9 = *M.getFunction("func9");

  // call void @func2()
  CallBase &F7FirstCB = static_cast<CallBase &>(*F7.getEntryBlock().begin());
  // call void @func2()
  Instruction &F9FirstInst = *F9.getEntryBlock().begin();
  // call void @func8
  Instruction &F9SecondInst = *++(F9.getEntryBlock().begin());

  const AAInterFnReachability &F1AA =
      *A.getOrCreateAAFor<AAInterFnReachability>(IRPosition::function(F1));

  const AAInterFnReachability &F6AA =
      *A.getOrCreateAAFor<AAInterFnReachability>(IRPosition::function(F6));

  const AAInterFnReachability &F7AA =
      *A.getOrCreateAAFor<AAInterFnReachability>(IRPosition::function(F7));

  const AAInterFnReachability &F9AA =
      *A.getOrCreateAAFor<AAInterFnReachability>(IRPosition::function(F9));

  F1AA.canReach(A, F3);
  F1AA.canReach(A, F4);
  F6AA.canReach(A, F4);
  F7AA.instructionCanReach(A, F7FirstCB, F3);
  F7AA.instructionCanReach(A, F7FirstCB, F4);
  F9AA.instructionCanReach(A, F9SecondInst, F3);
  F9AA.instructionCanReach(A, F9FirstInst, F3);
  F9AA.instructionCanReach(A, F9FirstInst, F4);

  A.run();

  ASSERT_TRUE(F1AA.canReach(A, F3));
  ASSERT_FALSE(F1AA.canReach(A, F4));

  ASSERT_TRUE(F7AA.instructionCanReach(A, F7FirstCB, F3));
  ASSERT_TRUE(F7AA.instructionCanReach(A, F7FirstCB, F4));

  // Assumed to be reacahable, since F6 can reach a function with
  // a unknown callee.
  ASSERT_TRUE(F6AA.canReach(A, F4));

  // The second instruction of F9 can't reach the first call.
  ASSERT_FALSE(F9AA.instructionCanReach(A, F9SecondInst, F3));

  // The first instruction of F9 can reach the first call.
  ASSERT_TRUE(F9AA.instructionCanReach(A, F9FirstInst, F3));
  // Because func10 calls the func4 after the call to func9 it is reachable but
  // as it requires backwards logic we would need AA::isPotentiallyReachable.
  ASSERT_FALSE(F9AA.instructionCanReach(A, F9FirstInst, F4));
}

TEST_F(AttributorTestBase, MemoryTransferAcrossLoopCall) {
  Module &M = parseModule(R"(
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
    define internal void @copy(ptr noalias %dst, ptr noalias %src) noinline {
      call void @llvm.memcpy.p0.p0.i64(ptr align 8 %dst, ptr align 8 %src,
                                     i64 16, i1 false)
      ret void
    }
    define ptr @chain(ptr %a, ptr %b, i1 %done) {
    entry:
      br label %loop
    loop:
      %src = phi ptr [ %a, %entry ], [ %dst, %loop ]
      %dst = phi ptr [ %b, %entry ], [ %src, %loop ]
      call void @copy(ptr %dst, ptr %src)
      br i1 %done, label %exit, label %loop
    exit:
      ret ptr %dst
    }
    define ptr @independent(ptr noalias %src, ptr noalias %dst, i1 %done) {
    entry:
      br label %loop
    loop:
      call void @copy(ptr %dst, ptr %src)
      br i1 %done, label %exit, label %loop
    exit:
      ret ptr %dst
    }
    declare void @unknown(ptr, ptr)
    define void @opaque(ptr %dst, ptr %src) {
      call void @unknown(ptr %dst, ptr %src)
      ret void
    }
  )");
  SetVector<Function *> Functions;
  for (Function &F : M)
    if (!F.isDeclaration())
      Functions.insert(&F);
  AnalysisGetter AG;
  CallGraphUpdater CGUpdater;
  BumpPtrAllocator Allocator;
  InformationCache InfoCache(M, AG, Allocator, nullptr);
  AttributorConfig AC(CGUpdater);
  AC.DeleteFns = false;
  AC.RewriteSignatures = false;
  Attributor A(Functions, InfoCache, AC);

  Function &F = *M.getFunction("chain");
  BasicBlock &Loop = *std::next(F.begin());
  auto &Call = cast<CallBase>(*Loop.getFirstNonPHIIt());
  Instruction *Transfer = &M.getFunction("copy")->getEntryBlock().front();
  const AAPointerInfo &DstInfo = *A.getOrCreateAAFor<AAPointerInfo>(
      IRPosition::callsite_argument(Call, 0));
  const AAPointerInfo &SrcInfo = *A.getOrCreateAAFor<AAPointerInfo>(
      IRPosition::callsite_argument(Call, 1));
  auto &UnknownCall =
      cast<CallBase>(M.getFunction("opaque")->getEntryBlock().front());
  const AAPointerInfo &UnknownInfo = *A.getOrCreateAAFor<AAPointerInfo>(
      IRPosition::callsite_argument(UnknownCall, 1));
  A.run();

  // The callee's access ranges and their originating transfer survive the
  // call boundary without inlining or changing the function signature.
  for (const AAPointerInfo *Info : {&DstInfo, &SrcInfo}) {
    ASSERT_TRUE(Info->getState().isValidState());
    unsigned Accesses = 0;
    EXPECT_TRUE(Info->forallInterferingAccesses(
        {0, 16}, [&](const AAPointerInfo::Access &Access, bool) {
          ++Accesses;
          EXPECT_EQ(Access.getLocalInst(), &Call);
          EXPECT_EQ(Access.getRemoteInst(), Transfer);
          EXPECT_EQ(Access.isWrite(), Info == &DstInfo);
          EXPECT_EQ(Access.isRead(), Info == &SrcInfo);
          EXPECT_TRUE(Access.hasUniqueRange());
          if (Access.hasUniqueRange()) {
            EXPECT_EQ(Access.getUniqueRange().Offset, 0);
            EXPECT_EQ(Access.getUniqueRange().Size, 16);
          }
          return true;
        }));
    EXPECT_EQ(Accesses, 1u);
    EXPECT_TRUE(Info->forallInterferingAccesses(
        {16, 8}, [&](const AAPointerInfo::Access &, bool) {
          ADD_FAILURE() << "Access outside the transfer range";
          return true;
        }));
  }

  // Translate the next iteration's source across the backedge. This is the
  // preceding iteration's destination, even though neither PHI is invariant.
  DominatorTree DT(F);
  PHITransAddr Source(Call.getArgOperand(1), M.getDataLayout(), nullptr);
  EXPECT_EQ(Source.translateValue(&Loop, &Loop, &DT, true),
            Call.getArgOperand(0));
  // The entry edge has no preceding transfer: it maps to the original source.
  PHITransAddr InitialSource(Call.getArgOperand(1), M.getDataLayout(), nullptr);
  EXPECT_EQ(InitialSource.translateValue(&Loop, &F.getEntryBlock(), &DT, true),
            F.getArg(0));
  EXPECT_NE(F.getArg(0), Call.getArgOperand(0));

  // Repeated calls alone do not establish the same recurrence.
  Function &Independent = *M.getFunction("independent");
  BasicBlock &IndependentLoop = *std::next(Independent.begin());
  DominatorTree IndependentDT(Independent);
  PHITransAddr IndependentSource(Independent.getArg(0), M.getDataLayout(),
                                 nullptr);
  EXPECT_EQ(IndependentSource.translateValue(&IndependentLoop, &IndependentLoop,
                                             &IndependentDT, true),
            Independent.getArg(0));
  EXPECT_NE(IndependentSource.getAddr(), Independent.getArg(1));
  // An unavailable callee cannot supply the transfer's access summary.
  EXPECT_FALSE(UnknownInfo.getState().isValidState());
}

} // namespace llvm
