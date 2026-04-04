//===-- c.dll dedicated DLL entry point ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c.dll's own _DllMainCRTStartup — NOT the generic section-walking
// dll_startup.cpp used by user DLLs. Uses explicit __libc_dll_init() for
// subsystem initialization instead of .CRT$XI* section dispatch.
//
// Key differences from dll_startup.cpp:
//   - Does NOT walk .CRT$XI* sections (subsystem init is explicit)
//   - DOES walk .CRT$XC* (C++ constructors — kept for safety)
//   - Calls __libc_dll_init() / __libc_dll_fini() for ordered init/fini
//   - Security cookie init is the first action (also covered by TLS
//     callback at .CRT$XLAA as defense-in-depth)
//
// Zero external includes — all Win32 types expressed as plain C types.
// This allows the file to compile freestanding, before any libc headers
// are available (early bootstrap phase).
//
//===----------------------------------------------------------------------===//

#include "startup/windows/dll_main_common.h"

// Tier A — OS floor check, PCB Zone 0, master VEH, identity, Zone 0 seal.
// Defined in libc_bootstrap.cpp. Runs before Tier B.
extern "C" void __libc_bootstrap();

// Tier B — allocator, fd table, signal dispatch, stdio skeleton, ...
// Defined in libc_subsystem_init.cpp / libc_subsystem_fini.cpp.
extern "C" int __libc_dll_init();
extern "C" void __libc_dll_fini();

// CRT section function pointer types.
using PVFV = void(*)(void); // C++ constructors/destructors

// .CRT$XC* section sentinels — C++ constructors in c.dll.
// The linker merges .CRT$XCA through .CRT$XCZ alphabetically.
// We still walk these to support any C++ globals in c.dll. Section flags
// are driven by the bookend variable types (extern const), so no
// #pragma section declaration is required.

extern "C" {
__LIBC_SECTION_ATTR(".CRT$XCA") __LIBC_SELECTANY_ATTR extern const PVFV __xc_a_dll[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XCZ") __LIBC_SELECTANY_ATTR extern const PVFV __xc_z_dll[] = {
    nullptr};
}

namespace {

void run_cpp_ctors() {
  for (const PVFV *fn = __xc_a_dll; fn < __xc_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

} // namespace

// c.dll never carries a user-supplied DllMain (it's a libc runtime, not
// user code), so the /alternatename fallback resolves to the default
// shim from dll_main_common.h.
LIBC_DLL_INSTALL_DEFAULT_DLLMAIN();

extern "C" LIBC_MSABI __LIBC_DLLEXPORT_ATTR int /* BOOL */
_DllMainCRTStartup(void *hinstDLL,          /* HINSTANCE */
                   unsigned long fdwReason,  /* DWORD */
                   void *lpvReserved) {      /* LPVOID */
  if (fdwReason == LIBC_DLL_PROCESS_ATTACH) {
    __dso_handle = hinstDLL;

    // Security cookie must be initialized before any /GS-protected function.
    // Also initialized by the .CRT$XLAA TLS callback as defense-in-depth,
    // but the TLS callback might not fire if crt_tls.obj is somehow missing.
    __security_init_cookie();

    // Tier A — must run before Tier B and before any C++ ctor. Brings
    // the process into a state where code can execute safely.
    __libc_bootstrap();

    // Tier B — explicit subsystem initialization, no .CRT$XI* section
    // walking. All ordering is source-visible in libc_subsystem_init.cpp.
    if (__libc_dll_init() != 0)
      return 0 /* FALSE */;

    // C++ constructors (.CRT$XC*) — kept for any C++ globals in c.dll.
    run_cpp_ctors();

    int result = DllMain(hinstDLL, fdwReason, lpvReserved);
    if (!result)
      return 0 /* FALSE */;

    return 1 /* TRUE */;
  }

  if (fdwReason == LIBC_DLL_PROCESS_DETACH) {
    // lpvReserved == nullptr → FreeLibrary (explicit unload).
    // lpvReserved != nullptr → process termination; skip cleanup.
    if (lpvReserved == nullptr) {
      __cxa_thread_finalize_dso_unload(__dso_handle);
      __cxa_finalize(__dso_handle);

      // Explicit subsystem shutdown — no .CRT$XP* section walking.
      __libc_dll_fini();
    }
  }

  return DllMain(hinstDLL, fdwReason, lpvReserved);
}
