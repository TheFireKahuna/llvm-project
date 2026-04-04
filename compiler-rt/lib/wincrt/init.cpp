//===-- init.cpp - Initialization and termination -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT initialization sequence for Windows Itanium.
//
// MUTUAL EXCLUSIVITY: This file defines symbols that conflict with vcruntime:
//   _cexit, _c_exit                 - Exit functions
//
// CRT section sentinels (__xi_a, __xi_z, __xc_a, __xc_z, __xp_a, __xp_z,
// __xt_a, __xt_z) and other mutual exclusivity symbols (__dso_handle,
// _fltused) are defined in builtins (crt_begin_windows.c / crt_end_windows.c)
// and linked early.
//
// Linking both wincrt and vcruntime will cause linker errors via:
// 1. #pragma detect_mismatch in internal.h
// 2. Multiple definition errors on these symbols
//
// Exit ordering per Itanium ABI and [basic.start.term]:
// 1. Thread-local destructors (__cxa_thread_finalize)
// 2. Static destructors in reverse registration order (__cxa_finalize)
// 3. Pre-terminators (.CRT$XPA-XPZ)
// 4. Terminators (.CRT$XTA-XTZ)
// 5. Stdio buffer flush (for _cexit compatibility with MSVC)
//
// Uses _register_thread_local_exe_atexit_callback (RS5+) to run Itanium
// cleanup before UCRT's atexit handlers during normal exit().
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "internal.h"

// UCRT imports. _initterm and _initterm_e are declared in <corecrt_startup.h>.
extern "C" {
__declspec(dllimport) int __cdecl fflush(void*);
__declspec(dllimport) unsigned int __cdecl _controlfp(unsigned int, unsigned int);
}


// Floating-point control word constants (from float.h).
namespace wincrt {
namespace fp {
constexpr unsigned int MCW_PC = 0x00030000;  // Precision control mask
constexpr unsigned int PC_53 = 0x00010000;   // 53-bit precision (x87)
} // namespace fp
} // namespace wincrt

extern "C" BOOL __stdcall _CRT_INIT(HINSTANCE hinstDLL, DWORD fdwReason,
                                    LPVOID lpvReserved) {
  (void)hinstDLL;

  if (fdwReason == DLL_PROCESS_ATTACH) {
    wincrt::securityInitCookie();
    if (_initterm_e(__xi_a, __xi_z) != 0)
      return FALSE;
    _initterm(__xc_a, __xc_z);
  } else if (fdwReason == DLL_PROCESS_DETACH) {
    // lpvReserved == nullptr means FreeLibrary (explicit unload).
    if (lpvReserved == nullptr) {
      // thread_local destructors before static destructors.
      __cxa_thread_finalize(__dso_handle);
      __cxa_finalize(__dso_handle);
      wincrt::runPreterminators();
      wincrt::runTerminators();
    }
  }

  return TRUE;
}

namespace wincrt {

namespace {

INIT_ONCE g_commonInitOnce = INIT_ONCE_STATIC_INIT;

BOOL __stdcall commonInitCallback(PINIT_ONCE, void *, void **) {
  verifyMinimumWindowsVersion();
  runPseudoRelocator();
  securityInitCookie();

  // Initialize floating-point state. _fpreset() sets control word to _CW_DEFAULT
  // and clears exception flags. On x86, also set 53-bit precision for x87 FPU
  // to match vcruntime behavior (prevents extended 64-bit precision surprises).
  _fpreset();
#if defined(__i386__)
  _controlfp(fp::PC_53, fp::MCW_PC);
#endif

  WINCRT_TRACE("running C initializers");
  if (_initterm_e(__xi_a, __xi_z) != 0)
    fatalError(RuntimeError::CrtNotInit);

  WINCRT_TRACE("running C++ constructors");
  _initterm(__xc_a, __xc_z);

  return TRUE;
}

} // namespace

void commonInit() {
  InitOnceExecuteOnce(&g_commonInitOnce, commonInitCallback, nullptr, nullptr);
}

void runPreterminators() {
  _initterm(__xp_a, __xp_z);
}

void runTerminators() {
  _initterm(__xt_a, __xt_z);
}

//===----------------------------------------------------------------------===//
// Exit synchronization
//===----------------------------------------------------------------------===//
//
// Prevents double-finalization when multiple exit paths converge:
// - exit() → cxaFinalizeBeforeAtexit callback
// - Direct _cexit() call
// - DLL unload via _CRT_INIT
//
// State machine: NOT_RUN → RUNNING → COMPLETED
// Only the thread that transitions NOT_RUN → RUNNING performs cleanup.
// Other threads wait for completion with bounded timeout.
//
// Uses SRWLOCK + CONDITION_VARIABLE (user-mode, no kernel objects):
// - Static zero-init is valid for both
// - SleepConditionVariableSRW provides bounded wait
// - WakeAllConditionVariable signals completion
//

namespace {

enum ExitState : int {
  EXIT_NOT_RUN = 0,
  EXIT_RUNNING = 1,
  EXIT_COMPLETED = 2
};

// Sync primitive wrappers matching Windows SDK layout (pointer-sized structs).
// Zero-initialization is valid for both.
// Cache-line aligned to prevent false sharing with hot paths.
struct alignas(CacheLineSize) ExitSync {
  int State;
  SRWLOCK Lock;
  CONDITION_VARIABLE Condition;
};

ExitSync g_exitSync = {EXIT_NOT_RUN, SRWLOCK_INIT, CONDITION_VARIABLE_INIT};

// Timeout for waiting on cleanup completion. 30 seconds is generous but
// prevents infinite hangs if a destructor deadlocks.
constexpr DWORD kExitCleanupTimeoutMs = 30000;

// Wrapper functions for type-safe calls to kernel32 imports.
void exitLockAcquire() {
  AcquireSRWLockExclusive(&g_exitSync.Lock);
}

void exitLockRelease() {
  ReleaseSRWLockExclusive(&g_exitSync.Lock);
}

} // namespace

bool tryBeginExitCleanup() {
  exitLockAcquire();

  if (g_exitSync.State == EXIT_NOT_RUN) {
    g_exitSync.State = EXIT_RUNNING;
    exitLockRelease();
    return true;
  }

  if (g_exitSync.State == EXIT_COMPLETED) {
    exitLockRelease();
    return false;
  }

  // Cleanup in progress by another thread. Wait with bounded timeout.
  // This handles the rare case of concurrent _cexit() calls.
  while (g_exitSync.State == EXIT_RUNNING) {
    BOOL ok = SleepConditionVariableSRW(&g_exitSync.Condition, &g_exitSync.Lock,
                                        kExitCleanupTimeoutMs, 0);
    if (!ok) {
      constexpr DWORD kErrorTimeout = 0x5B4; // ERROR_TIMEOUT
      if (GetLastError() == kErrorTimeout) {
        // Cleanup thread is stuck. Emit diagnostic and proceed.
        // This is a serious error but continuing is safer than hanging.
        WINCRT_FATAL("exit cleanup timed out after 30s - possible deadlock");
        break;
      }
      // Spurious wakeup or other error; recheck state.
    }
  }

  exitLockRelease();
  return false;
}

void runExitCleanup() {
  WINCRT_TRACE("running exit cleanup");

  // Per [basic.start.term]/1: thread_local destructors before static.
  __cxa_thread_finalize(nullptr);
  __cxa_finalize(nullptr);

  runPreterminators();
  runTerminators();

  // Mark complete and wake any waiting threads.
  exitLockAcquire();
  g_exitSync.State = EXIT_COMPLETED;
  exitLockRelease();

  WakeAllConditionVariable(&g_exitSync.Condition);
}

namespace {

// Callback for _register_thread_local_exe_atexit_callback.
// Runs before UCRT's atexit handlers during normal exit().
void __stdcall cxaFinalizeBeforeAtexit(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_DETACH) {
    if (tryBeginExitCleanup())
      runExitCleanup();
  }
}

#pragma section(".CRT$XIB", long, read)

int __cdecl installExitOrdering() {
  _register_thread_local_exe_atexit_callback(cxaFinalizeBeforeAtexit);
  return 0;
}

__declspec(allocate(".CRT$XIB")) static _PIFV g_initExitOrdering =
    installExitOrdering;

} // namespace

/// Query if termination has completed. Called from extern "C" wrapper.
bool isTerminationComplete() {
  return __atomic_load_n(&g_exitSync.State, __ATOMIC_ACQUIRE) == EXIT_COMPLETED;
}

} // namespace wincrt

extern "C" {

//===----------------------------------------------------------------------===//
// Termination status query
//===----------------------------------------------------------------------===//

/// Returns non-zero if CRT termination has completed.
/// Used by code that needs to check if cleanup is safe (e.g., exception
/// handling, thread-local destructors).
int __cdecl _is_c_termination_complete(void) {
  return wincrt::isTerminationComplete() ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Runtime error termination
//===----------------------------------------------------------------------===//

/// Runtime error messages matching MSVC _RT_* codes.
/// Format: "R60XX" where XX is the error number.
static const char *const g_runtimeErrorMessages[] = {
    nullptr,
    nullptr,
    "R6002\n- floating point not loaded\n",
    nullptr, nullptr, nullptr, nullptr, nullptr,
    "R6008\n- not enough space for arguments\n",
    "R6009\n- not enough space for environment\n",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "R6016\n- not enough space for thread data\n",
    "R6017\n- unexpected multithread lock error\n",
    "R6018\n- unexpected heap error\n",
    "R6019\n- unable to open console device\n",
    nullptr, nullptr, nullptr, nullptr,
    "R6024\n- not enough space for _onexit/atexit table\n",
    "R6025\n- pure virtual function call\n",
    "R6026\n- not enough space for stdio initialization\n",
    "R6027\n- not enough space for lowio initialization\n",
    "R6028\n- unable to initialize heap\n",
    nullptr,
    "R6030\n- CRT not initialized\n",
    "R6031\n- attempt to initialize CRT more than once\n",
    "R6032\n- not enough space for locale information\n",
    "R6033\n- attempt to use code from the CRT before initialization\n",
    "R6034\n- inconsistent onexit begin/end pointers\n",
};

static constexpr int kMaxRuntimeError =
    sizeof(g_runtimeErrorMessages) / sizeof(g_runtimeErrorMessages[0]) - 1;

/// Fatal runtime error. Displays message and terminates WITHOUT cleanup.
/// This is the fatal error path - no atexit handlers, no destructors.
/// Per MSVC: exits with code 255.
WINCRT_NORETURN void __cdecl _amsg_exit(int errnum) {
  // Write to stderr and OutputDebugString.
  const char *msg = nullptr;
  if (errnum >= 0 && errnum <= kMaxRuntimeError)
    msg = g_runtimeErrorMessages[errnum];

  OutputDebugStringA("\nruntime error ");
  if (msg) {
    OutputDebugStringA(msg);
    wincrt::writeStderr("\nruntime error ");
    wincrt::writeStderr(msg);
  } else {
    // Unknown error code - format as R60XX.
    char buf[32];
    char *p = buf;
    *p++ = 'R';
    *p++ = '6';
    *p++ = '0';
    // Simple decimal conversion.
    if (errnum < 0) {
      *p++ = '-';
      errnum = -errnum;
    }
    char digits[10];
    int ndigits = 0;
    int n = errnum;
    do {
      digits[ndigits++] = '0' + (n % 10);
      n /= 10;
    } while (n > 0);
    while (ndigits > 0)
      *p++ = digits[--ndigits];
    *p++ = '\n';
    *p = '\0';

    OutputDebugStringA(buf);
    wincrt::writeStderr("\nruntime error ");
    wincrt::writeStderr(buf);
  }

  // Fatal exit - NO cleanup, exit code 255.
  // Use _exit() to bypass all atexit handlers.
  _exit(255);
}

//===----------------------------------------------------------------------===//
// Normal exit functions
//===----------------------------------------------------------------------===//

// Complete CRT cleanup, then return (does not terminate process).
// Per MSVC: runs atexit handlers, flushes buffers, closes streams.
//
// Itanium ordering: thread_local destructors, then static destructors
// (registered via __cxa_atexit, including redirected atexit calls),
// then pre-terminators and terminators.
//
// Synchronized with cxaFinalizeBeforeAtexit to prevent double-finalization
// if both _cexit() and exit() are called.
void __cdecl _cexit(void) {
  if (wincrt::tryBeginExitCleanup())
    wincrt::runExitCleanup();

  // Flush stdio buffers per MSVC _cexit contract.
  // This runs even if cleanup was already done by another path,
  // since new output may have been written after cleanup.
  fflush(nullptr);
}

// Quick CRT cleanup, then return (does not terminate process).
// Per MSVC: no atexit handlers, no buffer flush.
void __cdecl _c_exit(void) {
  // Intentionally empty - matches MSVC _c_exit semantics.
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
