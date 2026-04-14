//===-- Unittests for psiginfo --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/psiginfo.h"

#include "hdr/stdint_proxy.h"
#include "test/UnitTest/Test.h"

TEST(LlvmLibcPsiginfoTest, PrintOut) {
  siginfo_t user_info = {};
  user_info.si_signo = SIGUSR1;
  user_info.si_code = 0;
  user_info.si_pid = 123;
  user_info.si_uid = 456;
  LIBC_NAMESPACE::psiginfo(&user_info, "user");

  siginfo_t child_info = {};
  child_info.si_signo = SIGCHLD;
  child_info.si_code = CLD_EXITED;
  child_info.si_pid = 321;
  child_info.si_uid = 654;
  child_info.si_status = 7;
  LIBC_NAMESPACE::psiginfo(&child_info, "child");

  siginfo_t fault_info = {};
  fault_info.si_signo = SIGSEGV;
  fault_info.si_code = 1;
  fault_info.si_addr = reinterpret_cast<void *>(static_cast<uintptr_t>(0x1234));
  LIBC_NAMESPACE::psiginfo(&fault_info, "fault");

  LIBC_NAMESPACE::psiginfo(nullptr, "null");
}
