//===- ABIContractTest.cpp - Physical ABI composition tests
//----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ABIContract.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::abi;

namespace {
Node record(StringRef Name) {
  return {Kind::Record, 0, Name.str(), {8, 8, 8, 8, 8, 0}, {}};
}

// The final field deliberately knows only A's identity, even when an earlier
// field knows its layout. Composition must preserve that distinction.
Contract requirements(unsigned Complete) {
  Contract C;
  C.Nodes.push_back(record("Root"));
  C.Nodes.push_back({Kind::Identity, 0, "A", {}, {}});
  C.Nodes.push_back({Kind::Identity, 0, "B", {}, {}});
  for (unsigned I = 0; I != 2; ++I) {
    uint32_t Target = I + 1;
    if (Complete & (1U << I)) {
      Target = C.Nodes.size();
      C.Nodes.push_back(record(I == 0 ? "A" : "B"));
    }
    C.Nodes[0].Edges.push_back(
        {Role::Field, Target, {I, I * 64, UINT64_MAX, 0}});
  }
  C.Nodes[0].Edges.push_back({Role::Field, 1, {2, 128, UINT64_MAX, 0}});
  return C;
}

TEST(ABIContractTest, ComposeComplementaryKnowledge) {
  for (unsigned A = 0; A != 4; ++A) {
    for (unsigned B = 0; B != 4; ++B) {
      Contract Left = requirements(A);
      Contract Right = requirements(B);
      auto Joined = Left.mergeWith(Right);
      ASSERT_TRUE(bool(Joined)) << toString(Joined.takeError());
      EXPECT_TRUE(Left.isSatisfiedBy(*Joined));
      EXPECT_TRUE(Right.isSatisfiedBy(*Joined));
      EXPECT_EQ(requirements(A | B).encode(), Joined->encode());
      auto Reverse = Right.mergeWith(Left);
      ASSERT_TRUE(bool(Reverse)) << toString(Reverse.takeError());
      EXPECT_EQ(Joined->encode(), Reverse->encode());
      auto Again = Joined->mergeWith(Left);
      ASSERT_TRUE(bool(Again)) << toString(Again.takeError());
      EXPECT_EQ(Joined->encode(), Again->encode());
    }
  }
}

TEST(ABIContractTest, RejectConflictingFacts) {
  Contract Left = requirements(1);
  Contract Right = requirements(2);
  Right.Nodes[0].Properties[0] += 8;
  auto Joined = Left.mergeWith(Right);
  EXPECT_FALSE(bool(Joined));
  consumeError(Joined.takeError());
  Right = requirements(2);
  Right.Nodes[0].Edges[0].Properties[1] += 8;
  Joined = Left.mergeWith(Right);
  EXPECT_FALSE(bool(Joined));
  consumeError(Joined.takeError());
  Right = requirements(2);
  Right.Nodes[1].Qualifiers = 1;
  Joined = Left.mergeWith(Right);
  EXPECT_FALSE(bool(Joined));
  consumeError(Joined.takeError());
}

TEST(ABIContractTest, SharedRecursiveDependencies) {
  Contract Left = requirements(1);
  Contract Right = requirements(2);
  for (Contract *C : {&Left, &Right}) {
    uint32_t Pointer = C->Nodes.size();
    C->Nodes.push_back(
        {Kind::Pointer, 0, "PRoot", {64, 64, 0}, {{Role::Pointee, 0, {}}}});
    C->Nodes[0].Edges.push_back(
        {Role::Field, Pointer, {3, 192, UINT64_MAX, 0}});
    // Repeat the same named physical type through a different path.
    C->Nodes[0].Edges.push_back(
        {Role::Field, C->Nodes[0].Edges[0].Target, {4, 256, UINT64_MAX, 0}});
    uint32_t VTable = C->Nodes.size();
    C->Nodes.push_back(
        {Kind::VTable, 0, "", {0}, {{Role::VTableEntry, 0, {0, 1, 0}}}});
    C->Nodes[0].Edges.push_back({Role::VTable, VTable, {}});
  }
  auto Joined = Left.mergeWith(Right);
  ASSERT_TRUE(bool(Joined)) << toString(Joined.takeError());
  EXPECT_TRUE(Left.isSatisfiedBy(*Joined));
  EXPECT_TRUE(Right.isSatisfiedBy(*Joined));
  auto Reverse = Right.mergeWith(Left);
  ASSERT_TRUE(bool(Reverse)) << toString(Reverse.takeError());
  EXPECT_EQ(Joined->encode(), Reverse->encode());
  auto Decoded = Contract::decode(Joined->encode());
  ASSERT_TRUE(bool(Decoded)) << toString(Decoded.takeError());
}
} // namespace
