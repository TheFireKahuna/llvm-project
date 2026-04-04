//===-- Unittests for psignal ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/psignal.h"

#include "test/UnitTest/Test.h"

#include <signal.h>

TEST(LlvmLibcPsignalTest, PrintOut) {
  LIBC_NAMESPACE::psignal(SIGUSR1, "A user signal");
  LIBC_NAMESPACE::psignal(SIGTERM, "");
  LIBC_NAMESPACE::psignal(SIGSEGV, nullptr);
  LIBC_NAMESPACE::psignal(99999, "An unknown signal");
}
