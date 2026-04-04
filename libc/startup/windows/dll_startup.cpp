//===-- DLL startup/shutdown for Windows ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Full DLL lifecycle for Windows Itanium. On DLL_PROCESS_ATTACH, runs
// .CRT$XI* (C initializers) and .CRT$XC* (C++ constructors). On
// DLL_PROCESS_DETACH (FreeLibrary, not process exit), runs thread-local
// and static destructors via __cxa_finalize(__dso_handle), then
// .CRT$XP* pre-terminators and .CRT$XT* terminators.
//
// Subsystems (file_pool, fd_table, signal_state) register their own init
// functions in .CRT$XI{B,C,D,E} via self-registration in their source
// files. The section walker here runs them in letter order. DLLs that
// don't reference a subsystem don't pull in its initializer — the linker
// only includes object files that resolve referenced symbols.
//
// User-defined DllMain is called via /alternatename fallback: if the DLL
// defines DllMain, the user's version wins. Otherwise, the default
// returns TRUE.
//
// Zero external includes — all Win32 types expressed as plain C types.
// This allows the file to compile freestanding, before any libc headers
// are available (early bootstrap phase).
//
//===----------------------------------------------------------------------===//

#include "startup/windows/dll_main_common.h"

// CRT section function pointer types.
using PIFV = int(*)(void);   /* C init — returns 0 on success */
using PVFV = void(*)(void);  /* C++ ctor / dtor */

// CRT section sentinel pairs — linker merges .CRT$X?A through .CRT$X?Z.
// Subsystems place their init/fini pointers between A and Z via section
// letter (e.g. .CRT$XIB for file_pool, .CRT$XIC for fd_table). Section
// flags are driven by the bookend variable types (extern const with
// constant-evaluable initializers → PSF_Read only → GV->setConstant
// true), so no #pragma section declaration is required.

extern "C" {
// C initializers (.CRT$XI*)
__LIBC_SECTION_ATTR(".CRT$XIA") __LIBC_SELECTANY_ATTR extern const PIFV __xi_a_dll[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XIZ") __LIBC_SELECTANY_ATTR extern const PIFV __xi_z_dll[] = {
    nullptr};

// C++ constructors (.CRT$XC*)
__LIBC_SECTION_ATTR(".CRT$XCA") __LIBC_SELECTANY_ATTR extern const PVFV __xc_a_dll[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XCZ") __LIBC_SELECTANY_ATTR extern const PVFV __xc_z_dll[] = {
    nullptr};

// Pre-terminators (.CRT$XP*)
__LIBC_SECTION_ATTR(".CRT$XPA") __LIBC_SELECTANY_ATTR extern const PVFV __xp_a_dll[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XPZ") __LIBC_SELECTANY_ATTR extern const PVFV __xp_z_dll[] = {
    nullptr};

// Terminators (.CRT$XT*)
__LIBC_SECTION_ATTR(".CRT$XTA") __LIBC_SELECTANY_ATTR extern const PVFV __xt_a_dll[] = {
    nullptr};
__LIBC_SECTION_ATTR(".CRT$XTZ") __LIBC_SELECTANY_ATTR extern const PVFV __xt_z_dll[] = {
    nullptr};

}

namespace {

int run_c_init() {
  for (const PIFV *fn = __xi_a_dll; fn < __xi_z_dll; ++fn) {
    if (*fn) {
      int result = (*fn)();
      if (result != 0)
        return result;
    }
  }
  return 0;
}

void run_cpp_init() {
  for (const PVFV *fn = __xc_a_dll; fn < __xc_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

void run_preterminators() {
  for (const PVFV *fn = __xp_a_dll; fn < __xp_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

void run_terminators() {
  for (const PVFV *fn = __xt_a_dll; fn < __xt_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

} // namespace

// User DLLs may supply their own DllMain; /alternatename gives them a
// no-op default if not. Both definitions live in dll_main_common.h.
LIBC_DLL_INSTALL_DEFAULT_DLLMAIN();

extern "C" LIBC_MSABI __LIBC_DLLEXPORT_ATTR int /* BOOL */
_DllMainCRTStartup(void *hinstDLL,          /* HINSTANCE */
                   unsigned long fdwReason,  /* DWORD */
                   void *lpvReserved) {      /* LPVOID */
  if (fdwReason == LIBC_DLL_PROCESS_ATTACH) {
    __dso_handle = hinstDLL;

    // Security cookie must be initialized before any /GS-protected function.
    __security_init_cookie();

    // Cross-DLL data references in RTTI are handled by dynamic
    // initialization (.CRT$XIB) + section sealing (.CRT$XIY).
    // Code-section references use .refptr. stubs collapsed by the linker.

    // C initializers (.CRT$XI*). Subsystems (file_pool, fd_table,
    // signal_state) self-register in .CRT$XI{B,C,D,E} — they run here
    // automatically when linked, in deterministic section-letter order.
    if (run_c_init() != 0)
      return 0 /* FALSE */;

    // C++ static constructors (.CRT$XC*).
    run_cpp_init();

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
      // Pre-terminators (.CRT$XP*) — subsystem fini registered here.
      run_preterminators();
      // Terminators (.CRT$XT*).
      run_terminators();
    }
  }

  return DllMain(hinstDLL, fdwReason, lpvReserved);
}
