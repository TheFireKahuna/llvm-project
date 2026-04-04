//===-- c.dll load/free resource-balance — parent test -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end check that __libc_dll_fini releases every TLS slot AND every
// kernel handle that __libc_dll_init claimed. A balanced init/fini pair is
// balanced at N=1 or never, so the child runs ONE warmup cycle to settle
// loader-side transients and ONE measurement cycle to check deltas — no
// 100× amplification loop.
//
// Why a child helper is unavoidable:
//   The parent test exe links c.dll (the LLVM libc test framework needs
//   libc symbols at link time). In-process LoadLibrary only bumps an
//   IAT-rooted refcount; FreeLibrary cannot drop to zero and
//   __libc_dll_fini never runs. The child is built ntdll-only, so a
//   LoadLibrary/FreeLibrary pair really drives PROCESS_ATTACH /
//   PROCESS_DETACH.
//
// Exit code (bitmask — multiple bits can be set if several things failed):
//   0x00  OK
//   0x01  LoadLibrary failed
//   0x02  FreeLibrary failed
//   0x04  PEB.TlsBitmap popcount drifted above baseline (tls_alloc / Tier B
//         subsystem leaked a slot across fini)
//   0x08  Process handle count drifted above baseline + slack (fini left
//         one or more kernel handles open)
//   0x10  NtQueryInformationProcess(ProcessHandleCount) failed
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"
#include "src/spawn/posix_spawn.h"
#include "src/sys/wait/waitpid.h"
#include "test/UnitTest/Test.h"

#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

#ifndef LIBC_DLL_CYCLE_RESOURCE_BALANCE_CHILD_PATH
#define LIBC_DLL_CYCLE_RESOURCE_BALANCE_CHILD_PATH                             \
  "libc_dll_cycle_resource_balance_child"
#endif

constexpr int kTlsLeakBit = 0x04;
constexpr int kHandleLeakBit = 0x08;

} // namespace

TEST(LlvmLibcDllCycleResourceBalance, InitFiniPairBalancesTlsAndHandles) {
  const char *child_path = LIBC_DLL_CYCLE_RESOURCE_BALANCE_CHILD_PATH;
  char *argv[] = {const_cast<char *>(child_path), nullptr};

  pid_t child_pid = 0;
  ASSERT_EQ(LIBC_NAMESPACE::posix_spawn(&child_pid, child_path,
                                        /*file_actions=*/nullptr,
                                        /*attrp=*/nullptr, argv, environ),
            0);

  int status = 0;
  ASSERT_EQ(LIBC_NAMESPACE::waitpid(child_pid, &status, 0), child_pid);
  ASSERT_TRUE(WIFEXITED(status))
      << "child died by signal — __libc_dll_init or __libc_dll_fini faulted";

  int code = WEXITSTATUS(status);
  // PEB.TlsBitmap drift after __libc_dll_fini — a Tier B subsystem claimed
  // a TLS slot that fini did not release.
  EXPECT_EQ(code & kTlsLeakBit, 0);
  // Process handle count drift after __libc_dll_fini — a .libcfin entry
  // left kernel handles open.
  EXPECT_EQ(code & kHandleLeakBit, 0);
  EXPECT_EQ(code, 0);
}
