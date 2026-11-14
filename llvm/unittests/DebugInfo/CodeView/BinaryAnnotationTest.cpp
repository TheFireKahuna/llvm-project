//===- BinaryAnnotationTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::codeview;

// What the encoder writes, BinaryAnnotationIterator reads back, at both ends
// of the one, two and four byte encodings.
TEST(BinaryAnnotationTest, RoundTrip) {
  const std::pair<uint32_t, size_t> Unsigned[] = {
      {0, 1}, {0x7f, 1}, {0x80, 2}, {0x3fff, 2}, {0x4000, 4}, {0x1fffffff, 4}};
  const std::pair<int32_t, size_t> Signed[] = {
      {0, 1},    {63, 1},    {-63, 1},  {64, 2},        {-64, 2},
      {8191, 2}, {-8191, 2}, {8192, 4}, {0xfffffff, 4}, {-0xfffffff, 4}};

  SmallVector<char, 64> Buffer;
  auto Encode = [&](BinaryAnnotationsOpCode Op, uint32_t Operand, size_t Size) {
    size_t Start = Buffer.size();
    EXPECT_TRUE(compressAnnotation(Op, Buffer));
    EXPECT_TRUE(compressAnnotation(Operand, Buffer));
    EXPECT_EQ(Buffer.size() - Start, 1 + Size);
  };
  for (auto [Value, Size] : Unsigned)
    Encode(BinaryAnnotationsOpCode::ChangeCodeOffset, Value, Size);
  for (auto [Value, Size] : Signed)
    Encode(BinaryAnnotationsOpCode::ChangeLineOffset, encodeSignedNumber(Value),
           Size);

  BinaryAnnotationIterator I(
      arrayRefFromStringRef(StringRef(Buffer.data(), Buffer.size())));
  for (auto [Value, Size] : Unsigned) {
    EXPECT_EQ((*I).OpCode, BinaryAnnotationsOpCode::ChangeCodeOffset);
    EXPECT_EQ((*I).U1, Value);
    ++I;
  }
  for (auto [Value, Size] : Signed) {
    EXPECT_EQ((*I).OpCode, BinaryAnnotationsOpCode::ChangeLineOffset);
    EXPECT_EQ((*I).S1, Value);
    ++I;
  }
  EXPECT_EQ(I, BinaryAnnotationIterator());

  // A value of more than 29 bits has no encoding.
  EXPECT_FALSE(compressAnnotation(0x20000000, Buffer));
}
