//===-- EXE startup for Windows (crt_do_start.obj) ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linked into the executable as crt_do_start.obj. Calls __libc_init (from
// c.dll) for internal libc setup, walks CRT init sections, calls main(),
// and exits. Faults inside main propagate to the master VEH — no frame-
// level SEH backstop.
//
// Separated from libc_init.cpp because this file references main() which
// only exists in the EXE, while libc_init.cpp uses internal libc symbols
// that live in c.dll.
//
//===----------------------------------------------------------------------===//

#include "startup/windows/do_start.h"
#include "src/__support/OSUtil/windows/ntdll.h"

extern "C" int main(int argc, char **argv, char **envp);

// Security cookie init — must run before any /GS-protected function.
extern "C" void __security_init_cookie(void);

// Per-module DSO handle for __cxa_atexit. For EXEs, the address of itself
// provides a unique identity. DLLs override with their HINSTANCE.
extern "C" __LIBC_SELECTANY_ATTR void *__dso_handle = &__dso_handle;

// Linker-emitted FP marker.
extern "C" __LIBC_SELECTANY_ATTR int _fltused = 0x9875;

// .CRT$X* section markers - linker merges in alphabetical order. Section
// attributes are derived from the bookend variable types below: each is
// `extern const` with a constant-evaluable initializer, so clang registers
// each section with PSF_Read only (no PSF_Write), which drives
// GV->setConstant(true) at IR emission — no #pragma section needed.
using PIFV = int(*)(void);  // C initializers (non-zero = failure)
using PVFV = void(*)(void); // C++ constructors/destructors

extern "C" {
// CRT section sentinels — selectany so tests linking individual .o files
// can include startup objects without duplicate-symbol errors.

// C initializers (.CRT$XI*)
__LIBC_SECTION_ATTR(".CRT$XIA") __LIBC_SELECTANY_ATTR extern const PIFV __xi_a[] = {nullptr};
__LIBC_SECTION_ATTR(".CRT$XIZ") __LIBC_SELECTANY_ATTR extern const PIFV __xi_z[] = {nullptr};

// C++ constructors (.CRT$XC*)
__LIBC_SECTION_ATTR(".CRT$XCA") __LIBC_SELECTANY_ATTR extern const PVFV __xc_a[] = {nullptr};
__LIBC_SECTION_ATTR(".CRT$XCZ") __LIBC_SELECTANY_ATTR extern const PVFV __xc_z[] = {nullptr};

// Pre-terminators (.CRT$XP*)
__LIBC_SECTION_ATTR(".CRT$XPA") __LIBC_SELECTANY_ATTR extern const PVFV __xp_a[] = {nullptr};
__LIBC_SECTION_ATTR(".CRT$XPZ") __LIBC_SELECTANY_ATTR extern const PVFV __xp_z[] = {nullptr};

// Terminators (.CRT$XT*)
__LIBC_SECTION_ATTR(".CRT$XTA") __LIBC_SELECTANY_ATTR extern const PVFV __xt_a[] = {nullptr};
__LIBC_SECTION_ATTR(".CRT$XTZ") __LIBC_SELECTANY_ATTR extern const PVFV __xt_z[] = {nullptr};
}

// Imported from c.dll — Tier A bootstrap (OS floor check, PCB Zone 0,
// master VEH, identity, seal Zone 0). Must run before any Tier B init,
// any user C initializer, or any C++ constructor.
extern "C" void __libc_bootstrap();

// Imported from c.dll — Tier B init (argv/env, TLS, allocator, fd table,
// signal dispatch, stdio skeleton). Internally invokes __libc_dll_init()
// to bring up all subsystems.
extern "C" void __libc_init(int *argc, char ***argv, char ***env);

// Imported from c.dll — public API.
extern "C" int atexit(void (*)(void));
extern "C" [[noreturn]] void exit(int status);

static void call_init_array_callbacks(int argc, char **argv, char **env) {
  (void)argc;
  (void)argv;
  (void)env;

  // Run C initializers (.CRT$XI*) — non-zero return means failure.
  for (const PIFV *fn = __xi_a; fn < __xi_z; ++fn) {
    if (*fn) {
      if ((*fn)() != 0)
        exit(255);
    }
  }

  // Run C++ constructors (.CRT$XC*)
  for (const PVFV *fn = __xc_a; fn < __xc_z; ++fn) {
    if (*fn)
      (*fn)();
  }
}

static void call_fini_array_callbacks() {
  // Run pre-terminators (.CRT$XP*)
  for (const PVFV *fn = __xp_a; fn < __xp_z; ++fn) {
    if (*fn)
      (*fn)();
  }

  // Run terminators (.CRT$XT*)
  for (const PVFV *fn = __xt_a; fn < __xt_z; ++fn) {
    if (*fn)
      (*fn)();
  }
}

extern "C" [[noreturn]] void __libc_do_start() {
  // Security cookie must be initialized before any /GS-protected function.
  __security_init_cookie();

  // Tier A — brings the process into a state where code can execute
  // safely (PCB Zone 0, master VEH, identity, Zone 0 seal). Must precede
  // any Tier B init and any C++ ctor. Idempotent: c.dll's DllMain has
  // already called this; our call is a no-op.
  __libc_bootstrap();

  // Cross-DLL data references in RTTI are handled by dynamic initialization
  // (.CRT$XIB) + section sealing (.CRT$XIY). Code-section references use
  // .refptr. stubs collapsed by the linker. No runtime pseudo-relocator needed.

  int argc;
  char **argv;
  char **env;
  __libc_init(&argc, &argv, &env);

  // Core subsystems (file_pool, fd_table, signal_state, std_fds, etc.)
  // are brought up explicitly by `__libc_dll_init()` (invoked above from
  // inside `__libc_init`), with PCB tier gating sequencing them
  // deterministically — no .CRT$XI* self-registration is used by libc
  // itself. The .CRT$XI{B..Y} and .CRT$XC* slots are reserved for
  // user-supplied C initializers and C++ constructors only, which run
  // after all libc subsystems are ready.
  atexit(&call_fini_array_callbacks);
  call_init_array_callbacks(argc, argv, env);

  int retval = main(argc, argv, env);
  exit(retval);
}
