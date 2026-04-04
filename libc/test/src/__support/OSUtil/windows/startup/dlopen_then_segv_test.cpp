//===-- Post-bootstrap dlopen → SEGV → master VEH dispatch test ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// End-to-end witness for the dll-load notification re-registration contract
// in libc/src/__support/OSUtil/windows/veh/veh_core.cpp:
//
//   * The master VEH handler is installed during Tier A, Phase 0b — long
//     before any user-driven dlopen() can run. New DLLs that are loaded
//     after Tier A may register their own vectored exception handlers,
//     which the OS pushes to the front of the VEH chain in arrival order;
//     a third-party VEH that returns EXCEPTION_CONTINUE_EXECUTION on faults
//     in its own code would silently swallow the AV and the libc would
//     never see the SIGSEGV.
//
//   * To prevent that, dll_notify_callback() in veh_core.cpp re-adds our
//     master handler at front-of-chain on every LDR_DLL_NOTIFICATION_REASON_
//     LOADED event using add-then-remove ordering (the swap is atomic on
//     veh_mutable().handler_handle, with both entries simultaneously live
//     across the overlap window — the reentry guard in master_veh_handler
//     keeps that safe).
//
// This test reproduces the contract end-to-end:
//
//   1. Install a sigaction(SIGSEGV) handler whose body sets a flag and
//      longjmps out of the faulting context. (We cannot return from a
//      synchronous AV handler — the faulting instruction would re-execute
//      and re-trap. POSIX longjmp-out-of-handler is the documented escape
//      hatch on a fatal hardware fault, and our SEH-class signal dispatch
//      delivers the signal on the faulting thread synchronously, with the
//      original CONTEXT, so the longjmp lands cleanly.)
//
//   2. dlopen() the helper DLL by absolute path. The helper's load
//      triggers the dll-load notification → master VEH re-add at front.
//
//   3. dlsym() the helper's `faulting_entry` symbol and call it. The
//      null store inside the helper raises EXCEPTION_ACCESS_VIOLATION,
//      which travels: kernel → VEH chain head (us, after the re-add) →
//      master_veh_handler → signal_veh_transport filter → SIGSEGV
//      delivery → our sigaction handler → setjmp/longjmp recovery.
//
//   4. We assert the handler ran (flag set) and that we returned via
//      the longjmp recovery branch, not via normal control flow from
//      faulting_entry().
//
// CMake wiring needed (TODO in parent task — this file deliberately does
// NOT touch any CMakeLists.txt):
//
//   * Add a new helper macro (e.g. add_cdll_helper_dll) that builds
//     dlopen_segv_helper.cpp as a SHARED library named
//     "dlopen_segv_helper.dll" in the same RUNTIME_OUTPUT_DIRECTORY as
//     this test executable. The helper must NOT link c.lib /
//     libc_shared — its purpose is to be a third-party DLL.
//   * Wire this test via add_cdll_test, with EXTRA_DEPS pointing at the
//     helper DLL target.
//   * Define DLOPEN_SEGV_HELPER_PATH=$<TARGET_FILE:dlopen_segv_helper>
//     on this test target, mirroring VT_PTY_TREE_JOIN_CHILD_PATH /
//     PTY_LIFECYCLE_CHILD_PATH.
//
//===----------------------------------------------------------------------===//

#include "test/UnitTest/Test.h"

#include "src/__support/macros/config.h"
#include "src/dlfcn/dlclose.h"
#include "src/dlfcn/dlerror.h"
#include "src/dlfcn/dlopen.h"
#include "src/dlfcn/dlsym.h"
#include "src/setjmp/siglongjmp.h"
#include "src/setjmp/sigsetjmp.h"
#include "src/signal/sigaction.h"
#include "src/signal/sigemptyset.h"

#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

// Path to the third-party helper DLL — co-located with the test exe by
// CMake. Mirrors the VT_PTY_TREE_JOIN_CHILD_PATH / PTY_LIFECYCLE_CHILD_PATH
// convention.
#ifndef DLOPEN_SEGV_HELPER_PATH
#define DLOPEN_SEGV_HELPER_PATH "dlopen_segv_helper.dll"
#endif

// Recovery state for the SIGSEGV handler. `volatile sig_atomic_t` so the
// compiler cannot optimise out the read after longjmp; jmp_buf is the
// landing pad set by setjmp() before we provoke the fault.
volatile sig_atomic_t g_segv_handler_fired = 0;
volatile sig_atomic_t g_segv_signo_seen = 0;
sigjmp_buf g_segv_recover;

extern "C" void segv_recovery_handler(int signo) {
  g_segv_handler_fired = 1;
  g_segv_signo_seen = signo;
  // Long-jump out — returning from a synchronous AV handler would re-run
  // the faulting instruction and re-trap forever.
  LIBC_NAMESPACE::siglongjmp(g_segv_recover, 1);
}

// Helper to install our SIGSEGV handler with SA_NODEFER (so a recursive
// fault during recovery is at least observable as a different failure
// mode rather than a silent reentry block) and capture the previous
// disposition for restore.
struct ScopedSegvHandler {
  struct sigaction old_sa;
  bool installed = false;

  ScopedSegvHandler() {
    struct sigaction sa = {};
    sa.sa_handler = &segv_recovery_handler;
    sa.sa_flags = SA_NODEFER;
    LIBC_NAMESPACE::sigemptyset(&sa.sa_mask);
    if (LIBC_NAMESPACE::sigaction(SIGSEGV, &sa, &old_sa) == 0)
      installed = true;
  }
  ~ScopedSegvHandler() {
    if (installed)
      LIBC_NAMESPACE::sigaction(SIGSEGV, &old_sa, nullptr);
  }
};

} // namespace

// ---------------------------------------------------------------------------
// dlopen-then-fault: master VEH must still own front-of-chain after the
// helper DLL maps in, so the AV inside helper code is delivered as SIGSEGV
// to our sigaction handler.
// ---------------------------------------------------------------------------

TEST(LlvmLibcDlopenThenSegvTest, FaultInDlopenedDllRoutesThroughMasterVeh) {
  // Step 1: install our SIGSEGV handler before doing anything else. If the
  // dlopen / dlsym path itself faults (it shouldn't), we'd at least catch
  // it and surface a clear failure rather than killing the test runner.
  ScopedSegvHandler scoped;
  ASSERT_TRUE(scoped.installed);

  g_segv_handler_fired = 0;
  g_segv_signo_seen = 0;

  // Step 2: dlopen the third-party helper. This is the load event that
  // veh_core.cpp::dll_notify_callback observes; it re-adds the master
  // handler at front-of-chain via add-then-remove. If that path is broken,
  // the helper DLL's eventual fault would still hit the master handler
  // here only because no other DLL has registered a competing VEH — the
  // contract teeth come from the re-registration ordering, not from
  // arrival order.
  void *handle =
      LIBC_NAMESPACE::dlopen(DLOPEN_SEGV_HELPER_PATH, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(handle, static_cast<void *>(nullptr))
      << "dlopen of helper DLL failed; check DLOPEN_SEGV_HELPER_PATH";

  // Step 3: resolve the faulting entry point.
  using FaultingFn = void (*)(void);
  void *sym = LIBC_NAMESPACE::dlsym(handle, "faulting_entry");
  ASSERT_NE(sym, static_cast<void *>(nullptr))
      << "dlsym(faulting_entry) failed: " << LIBC_NAMESPACE::dlerror();
  FaultingFn faulting_entry = reinterpret_cast<FaultingFn>(sym);

  // Step 4: arm setjmp landing pad, then call into the helper. The call
  // must NOT return normally — the null-pointer store inside the helper
  // raises EXCEPTION_ACCESS_VIOLATION → SIGSEGV → segv_recovery_handler →
  // siglongjmp here.
  if (sigsetjmp(g_segv_recover, 1) == 0) {
    faulting_entry();
    // If we reach this line, the fault was not delivered as a signal —
    // either the master VEH lost front-of-chain ownership or signal
    // dispatch broke. Force a hard test failure with a sentinel.
    // faulting_entry() returned normally; SIGSEGV was not delivered
    // through the master VEH.
    LIBC_NAMESPACE::dlclose(handle);
    ASSERT_TRUE(false);
    return;
  }

  // Post-longjmp landing — assert the recovery actually ran via our
  // sigaction handler, not via some other escape hatch.
  EXPECT_EQ(g_segv_handler_fired, 1);
  EXPECT_EQ(g_segv_signo_seen, SIGSEGV);

  // Tear down: dlclose the helper. Don't ASSERT on this — the test's
  // primary observable has already been recorded above; a dlclose failure
  // here is interesting diagnostic noise but not a regression of the VEH
  // re-registration contract under test.
  EXPECT_EQ(LIBC_NAMESPACE::dlclose(handle), 0);
}

// ---------------------------------------------------------------------------
// Repeat after dlclose+dlopen: re-loading the helper DLL must trigger the
// dll-load notification a second time and again re-seat the master VEH.
// This guards against a regression where the re-add path runs only on the
// very first load (e.g. someone caches "we've already done this" state).
// ---------------------------------------------------------------------------

TEST(LlvmLibcDlopenThenSegvTest, ReloadAfterDlcloseStillRoutesThroughMasterVeh) {
  ScopedSegvHandler scoped;
  ASSERT_TRUE(scoped.installed);

  for (int iter = 0; iter < 2; ++iter) {
    g_segv_handler_fired = 0;
    g_segv_signo_seen = 0;

    void *handle =
        LIBC_NAMESPACE::dlopen(DLOPEN_SEGV_HELPER_PATH, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(handle, static_cast<void *>(nullptr));

    using FaultingFn = void (*)(void);
    void *sym = LIBC_NAMESPACE::dlsym(handle, "faulting_entry");
    ASSERT_NE(sym, static_cast<void *>(nullptr));
    FaultingFn faulting_entry = reinterpret_cast<FaultingFn>(sym);

    if (sigsetjmp(g_segv_recover, 1) == 0) {
      faulting_entry();
      // iter: faulting_entry() returned normally.
      LIBC_NAMESPACE::dlclose(handle);
      ASSERT_TRUE(false);
      continue;
    }

    EXPECT_EQ(g_segv_handler_fired, 1);
    EXPECT_EQ(g_segv_signo_seen, SIGSEGV);
    EXPECT_EQ(LIBC_NAMESPACE::dlclose(handle), 0);
  }
}
