//===-- Unittests for AllocChecker ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/new.h"
#include "src/__support/alloc-checker.h"
#include "src/string/memset.h"
#include "test/UnitTest/Test.h"

// An aligned allocation holds the requested size at the requested alignment,
// and every aligned form of operator delete releases it.
TEST(LlvmLibcAllocCheckerTest, AlignedNewAndDelete) {
  constexpr size_t SIZE = 1024;
  constexpr size_t ALIGN = 64;
  for (int form = 0; form < 4; ++form) {
    LIBC_NAMESPACE::AllocChecker ac;
    void *mem = form < 2 ? ::operator new(SIZE, std::align_val_t(ALIGN), ac)
                         : ::operator new[](SIZE, std::align_val_t(ALIGN), ac);
    ASSERT_TRUE(static_cast<bool>(ac));
    ASSERT_EQ(reinterpret_cast<uintptr_t>(mem) % ALIGN, uintptr_t(0));
    LIBC_NAMESPACE::memset(mem, 0x5a, SIZE);
    switch (form) {
    case 0:
      ::operator delete(mem, std::align_val_t(ALIGN));
      break;
    case 1:
      ::operator delete(mem, SIZE, std::align_val_t(ALIGN));
      break;
    case 2:
      ::operator delete[](mem, std::align_val_t(ALIGN));
      break;
    case 3:
      ::operator delete[](mem, SIZE, std::align_val_t(ALIGN));
      break;
    }
  }
}
