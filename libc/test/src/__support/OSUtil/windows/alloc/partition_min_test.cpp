//===-- partition_min_test.cpp - leaf-link smoke for alloc::partition -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Minimum-viable hermetic smoke test for the partition translation unit.
// The full quality / stress matrix lives in partition_test.cpp; this file
// exists only to prove that
// `libc.src.__support.OSUtil.windows.partition` links in isolation —
// pulling the symbol's address through a volatile sink forces the linker
// to resolve `reserve_or_grow` and defeats DCE, so a regression that
// breaks leaf linkage (missing dependency, ODR conflict, undefined
// reference) fails the build instead of silently passing.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/partition.h"
#include "test/UnitTest/Test.h"

namespace partition = LIBC_NAMESPACE::windows::alloc::partition;

TEST(LlvmLibcPartitionMinTest, PartitionLeafLinks) {
  void *volatile sink = reinterpret_cast<void *>(&partition::reserve_or_grow);
  EXPECT_NE(sink, static_cast<void *>(nullptr));
}
