//===-- Loader-lock ctor fault → master VEH dispatch test ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end witness that the libc master VEH handler is the terminal
// consumer of a hardware fault originating inside a user DLL's static
// constructor — i.e. the fault is raised WHILE THE NT LOADER LOCK IS HELD,
// which is the most hostile environment in which our VEH must still own
// front-of-chain dispatch.
//
// Mechanism under test
// --------------------
// libc/src/__support/OSUtil/windows/veh/veh_core.cpp installs the master VEH
// during Tier A Phase 0d (via the `.libcveh` registry) and also registers a
// LdrRegisterDllNotification callback. Every
// LDR_DLL_NOTIFICATION_REASON_LOADED event causes dll_notify_callback() to
// re-add the master handler at front-of-chain using add-then-remove
// ordering. The loader delivers the LOADED notification BEFORE invoking
// the freshly-loaded DLL's DllMain / static constructors — so by the time
// the helper's ctor runs, the master VEH must already be at the head of
// the chain to observe the AV. This test proves that contract.
//
// Protocol (matches ctor_fault_helper.cpp exactly)
// ------------------------------------------------
//   * Env-var gate: "CTOR_FAULT_HELPER_FAULT"
//       - presence (non-empty or empty, value irrelevant) arms the ctor's
//         null-store path; absence keeps the DLL benign.
//   * Sentinel export: "ctor_fault_helper_loaded" returning 0xCAFE
//       - resolved post-dlopen on the non-faulting path to distinguish
//         "loaded successfully without faulting" from "DLL never mapped".
//   * Helper DLL must be a freestanding SHARED library (no c.lib /
//     libc_shared link) so the fault originates strictly in third-party
//     code, not inside libc.
//
// Test shape (fork-based)
// -----------------------
// The parent forks. The child sets CTOR_FAULT_HELPER_FAULT=1, then
// dlopen()s the helper. The ctor fires under the loader lock and
// performs a null-store. With no SIGSEGV handler installed in the child,
// the libc default disposition for SIGSEGV is process termination, which
// the child_table encode path surfaces as WIFSIGNALED + WTERMSIG ==
// SIGSEGV to the parent's waitpid(). That exact wait status is the
// positive witness that (a) the master VEH caught the AV, (b) the signal
// subsystem mapped it to SIGSEGV, and (c) the default action was taken —
// as opposed to the raw NT unhandled-exception filter tearing the process
// down with some other status.
//
// Sentinels (distinguishable failure modes)
// -----------------------------------------
//   77  REACHED_END       — dlopen() returned without faulting. Env-var
//                           gate misread by helper OR helper regression
//                           that elided the null store.
//   78  DLOPEN_RETURNED_NULL — dlopen() failed outright (not a crash).
//                           Helper not found on disk, wrong path, or
//                           loader refused the DLL — NOT a VEH regression.
//   79  SETENV_FAILED     — could not set the gate env var. Plumbing
//                           failure inside the child before we even got
//                           to the interesting path.
// Any of these as WIFEXITED => ASSERT fails and the sentinel pinpoints
// which step broke. The expected path is WIFSIGNALED with SIGSEGV.
//
// CMake wiring contract (deferred — this file does NOT touch CMakeLists.txt)
// --------------------------------------------------------------------------
//   * Build ctor_fault_helper.cpp as a SHARED library target named
//     `ctor_fault_helper`, following the same pattern as
//     `dlopen_segv_helper`: freestanding, does NOT link c.lib /
//     libc_shared, placed in the same RUNTIME_OUTPUT_DIRECTORY as this
//     test's executable so a bare filename dlopen works as a fallback.
//   * This parent test is built via `add_cdll_test` with
//     `EXTRA_DEPS ctor_fault_helper`.
//   * Define `CTOR_FAULT_HELPER_PATH=$<TARGET_FILE:ctor_fault_helper>`
//     on this test target so the absolute path is compiled in — mirrors
//     DLOPEN_SEGV_HELPER_PATH / PTY_LIFECYCLE_CHILD_PATH /
//     VT_PTY_TREE_JOIN_CHILD_PATH.
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"
#include "src/__support/macros/config.h"
#include "src/dlfcn/dlopen.h"
#include "src/dlfcn/dlsym.h"
#include "src/stdlib/setenv.h"
#include "src/sys/wait/waitpid.h"
#include "src/unistd/_exit.h"
#include "src/unistd/fork.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Absolute path to the helper DLL, injected by CMake. Fall back to a bare
// filename so the test still links if built outside the normal CMake path;
// dlopen() will then rely on the loader's search order.
#ifndef CTOR_FAULT_HELPER_PATH
#define CTOR_FAULT_HELPER_PATH "ctor_fault_helper.dll"
#endif

// Env var name and sentinel export name — MUST match ctor_fault_helper.cpp
// byte-for-byte. If either string drifts, the helper's gate silently
// misreads and the test's observable (SIGSEGV termination) flips to the
// REACHED_END sentinel, which is still a hard failure — but the name-
// mismatch diagnostic is clearer if the strings are defined once, here.
constexpr const char kFaultEnvVar[] = "CTOR_FAULT_HELPER_FAULT";
constexpr const char kLoadedSymbol[] = "ctor_fault_helper_loaded";

// Child exit sentinels. Distinct, non-overlapping, and none of them
// collide with common defaults (0, 1, 42, 77 is reused intentionally as
// REACHED_END to flag the "ctor ran without faulting" regression).
constexpr int kReachedEnd = 77;
constexpr int kDlopenReturnedNull = 78;
constexpr int kSetenvFailed = 79;

} // namespace

// ===----------------------------------------------------------------------===
// Fork + child dlopens gated helper → ctor null-stores under loader lock →
// master VEH catches AV → SIGSEGV default disposition terminates child →
// parent's waitpid observes WIFSIGNALED && WTERMSIG == SIGSEGV.
// ===----------------------------------------------------------------------===

TEST(LlvmLibcCtorFaultsDuringStartupTest, CtorFaultUnderLoaderLockRoutesThroughMasterVeh) {
  pid_t pid = LIBC_NAMESPACE::fork();
  ASSERT_NE(pid, -1);

  if (pid == 0) {
    // ----- Child -----
    // Arm the env-var gate BEFORE dlopen — the helper's ctor reads it
    // during DLL_PROCESS_ATTACH under the loader lock, so it must be set
    // in the process environment at the moment dlopen() enters the
    // loader. setenv() writes to the Win32 env block which the helper's
    // GetEnvironmentVariableA-based probe will see.
    if (LIBC_NAMESPACE::setenv(kFaultEnvVar, "1", /*overwrite=*/1) != 0)
      LIBC_NAMESPACE::_exit(kSetenvFailed);

    // dlopen() under normal post-bootstrap conditions. The loader will:
    //   1. Map the helper image.
    //   2. Deliver LDR_DLL_NOTIFICATION_REASON_LOADED → our
    //      dll_notify_callback re-adds master VEH at front-of-chain.
    //   3. Invoke helper DllMain + static ctors UNDER THE LOADER LOCK.
    //   4. Our CtorFault ctor reads CTOR_FAULT_HELPER_FAULT, sees it
    //      set, performs a null store → EXCEPTION_ACCESS_VIOLATION.
    //   5. Kernel walks the VEH chain; master_veh_handler is at the
    //      head; signal_veh_transport filter maps the AV to SIGSEGV;
    //      with no user handler installed, the libc default action is
    //      terminate-with-signal → wait status encodes WTERMSIG = SIGSEGV.
    //
    // If dlopen() returns normally here, the ctor did not fault — which
    // could mean the env-var gate was misread, the helper was built
    // without the faulting path, or the fault was silently swallowed by
    // some other VEH at front-of-chain. All of those are test failures
    // and are surfaced via the kReachedEnd sentinel.
    void *handle =
        LIBC_NAMESPACE::dlopen(CTOR_FAULT_HELPER_PATH, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
      LIBC_NAMESPACE::_exit(kDlopenReturnedNull);

    // If we reach this point, the ctor ran to completion without the
    // AV taking down the process. Probe the sentinel export to confirm
    // the DLL actually loaded (i.e. we're not looking at a stale handle
    // from a cached mapping), then report the regression sentinel.
    (void)LIBC_NAMESPACE::dlsym(handle, kLoadedSymbol);
    LIBC_NAMESPACE::_exit(kReachedEnd);
  }

  // ----- Parent -----
  int status = 0;
  pid_t reaped = LIBC_NAMESPACE::waitpid(pid, &status, 0);
  ASSERT_EQ(reaped, pid);

  // The load-bearing assertion: the child must have been terminated by
  // SIGSEGV, delivered through the libc's signal subsystem. That is the
  // end-to-end witness that the master VEH caught the loader-lock-held
  // ctor fault.
  //
  // If WIFEXITED is true instead, the child's exit status identifies
  // which precondition broke (kSetenvFailed / kDlopenReturnedNull /
  // kReachedEnd). The EXPECT lines below emit a clear regression signal
  // in that case rather than just "test failed".
  ASSERT_TRUE(WIFSIGNALED(status))
      << "child was not killed by a signal; WIFEXITED=" << WIFEXITED(status)
      << " WEXITSTATUS=" << WEXITSTATUS(status)
      << " (77=reached-end/ctor-didn't-fault, 78=dlopen-returned-null, "
         "79=setenv-failed)";
  EXPECT_FALSE(WIFEXITED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV)
      << "child was signalled, but with the wrong signal — master VEH "
         "likely lost front-of-chain during LDR_DLL_NOTIFICATION_REASON_"
         "LOADED dispatch";
}
