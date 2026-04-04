//===-- dlopen-then-SEGV helper DLL ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helper DLL for dlopen_then_segv_test. The parent test dlopen()'s this
// module *after* libc bootstrap has installed the master VEH, then calls
// `faulting_entry()` via dlsym. The body deliberately performs a null-pointer
// store; the resulting EXCEPTION_ACCESS_VIOLATION is the witness used by the
// parent to assert that the master VEH still owns front-of-chain dispatch
// after a third-party DLL load (the dll-load notification re-registers via
// add-then-remove, see veh_core.cpp::dll_notify_callback).
//
// CMake wiring needed (TODO in parent task — this file deliberately does NOT
// touch any CMakeLists.txt):
//
//   * Build this source as a SHARED library / MODULE named
//     "dlopen_segv_helper.dll", placed alongside the test executable in the
//     same RUNTIME_OUTPUT_DIRECTORY (mirroring the pty_lifecycle_child /
//     vt_pty_tree_join_child pattern, but as a .dll rather than a .exe — so
//     a new add_cdll_helper_dll macro will be needed in the directory's
//     CMakeLists.txt).
//   * The DLL must NOT link c.lib / libc_shared. Keep it freestanding so the
//     fault originates in third-party code, not in libc.
//   * The parent test target must depend on this DLL and define
//     DLOPEN_SEGV_HELPER_PATH=$<TARGET_FILE:dlopen_segv_helper>, mirroring
//     the VT_PTY_TREE_JOIN_CHILD_PATH / PTY_LIFECYCLE_CHILD_PATH pattern.
//
// The body intentionally has no dependencies — no libc, no CRT, no C++
// runtime. A bare exported function + a null-pointer store. Marked
// `volatile` so the optimizer cannot prove the store is dead and elide it.
//
//===----------------------------------------------------------------------===//

#define HELPER_EXPORT [[gnu::dllexport]]

extern "C" HELPER_EXPORT void faulting_entry(void) {
  // Null-pointer store — raises EXCEPTION_ACCESS_VIOLATION (mapped to
  // VEH_ACCESS_VIOLATION, which the signal_veh_transport filter routes to
  // SIGSEGV). The store target is `volatile` so the compiler must emit it.
  *static_cast<volatile int *>(nullptr) = 0;
}

// Some toolchains require an explicit DllMain for a bare DLL with no CRT.
// Returning TRUE unconditionally is sufficient — we have no per-process or
// per-thread state to set up.
#if defined(_WIN32) || defined(_WIN32_ITANIUM) || defined(__NTPOSIX__)
extern "C" int DllMain(void * /*hinst*/, unsigned long /*reason*/,
                                 void * /*reserved*/) {
  return 1;
}
#endif
