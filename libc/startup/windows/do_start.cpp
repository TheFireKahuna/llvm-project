//===-- EXE startup for Windows (crt_do_start.obj) ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linked into the executable as crt_do_start.obj. Calls __libc_init (from
// c.dll) for internal libc setup, walks CRT init sections, calls main()
// via an SEH-guarded wrapper, and exits.
//
// Separated from libc_init.cpp because this file references main() which
// only exists in the EXE, while libc_init.cpp uses internal libc symbols
// that live in c.dll.
//
//===----------------------------------------------------------------------===//

#include "startup/windows/do_start.h"
#include "src/__support/OSUtil/windows/ntdll.h"

extern "C" int main(int argc, char **argv, char **envp);

// SEH-guarded main() wrapper. The .seh_handler provides demand-commit and
// remap-guard fault recovery as defense-in-depth if VEH is displaced by a
// third-party DLL's handler. Uses the EXCEPTION_ROUTINE interface — fully
// transparent to libunwind's RtlVirtualUnwind stack walk.
extern "C" LONG NTAPI __llvm_libc_thread_fault_handler(
    EXCEPTION_RECORD *, void *, CONTEXT *, DISPATCHER_CONTEXT *);

#if defined(__x86_64__)
// x86_64: main(argc, argv, envp) takes three args in rcx, rdx, r8.
extern "C" [[gnu::naked]]
int call_main_guarded(int /*argc*/, char ** /*argv*/, char ** /*envp*/) {
  asm volatile(R"(
      .seh_proc call_main_guarded
      .seh_handler __llvm_libc_thread_fault_handler, @except
      push %rbp
      .seh_pushreg %rbp
      mov %rsp, %rbp
      .seh_setframe %rbp, 0
      sub $32, %rsp
      .seh_stackalloc 32
      .seh_endprologue
      call main
      add $32, %rsp
      pop %rbp
      retq
      .seh_endproc
  )");
}
#elif defined(__aarch64__)
// aarch64: main(argc, argv, envp) takes three args in x0, x1, x2.
extern "C" [[gnu::naked]]
int call_main_guarded(int /*argc*/, char ** /*argv*/, char ** /*envp*/) {
  asm volatile(R"(
      .seh_proc call_main_guarded
      .seh_handler __llvm_libc_thread_fault_handler, @except
      pacibsp
      .seh_pac_sign_lr
      stp x29, x30, [sp, #-16]!
      .seh_save_fplr_x 16
      mov x29, sp
      .seh_set_fp
      .seh_endprologue
      bl main
      .seh_startepilogue
      ldp x29, x30, [sp], #16
      .seh_save_fplr_x 16
      autibsp
      .seh_pac_sign_lr
      .seh_endepilogue
      ret
      .seh_endproc
  )");
}
#endif

// Security cookie init — must run before any /GS-protected function.
extern "C" void __security_init_cookie(void);

// Per-module DSO handle for __cxa_atexit. For EXEs, the address of itself
// provides a unique identity. DLLs override with their HINSTANCE.
extern "C" __declspec(selectany) void *__dso_handle = &__dso_handle;

// Linker-emitted FP marker.
extern "C" __declspec(selectany) int _fltused = 0x9875;

// .CRT$X* section markers - linker merges in alphabetical order.
using PIFV = int(__cdecl *)(void);  // C initializers (non-zero = failure)
using PVFV = void(__cdecl *)(void); // C++ constructors/destructors

#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
#pragma section(".CRT$XPA", long, read)
#pragma section(".CRT$XPZ", long, read)
#pragma section(".CRT$XTA", long, read)
#pragma section(".CRT$XTZ", long, read)

extern "C" {
// CRT section sentinels — selectany so tests linking individual .o files
// can include startup objects without duplicate-symbol errors.

// C initializers (.CRT$XI*)
__declspec(allocate(".CRT$XIA")) __declspec(selectany) PIFV __xi_a[] = {nullptr};
__declspec(allocate(".CRT$XIZ")) __declspec(selectany) PIFV __xi_z[] = {nullptr};

// C++ constructors (.CRT$XC*)
__declspec(allocate(".CRT$XCA")) __declspec(selectany) PVFV __xc_a[] = {nullptr};
__declspec(allocate(".CRT$XCZ")) __declspec(selectany) PVFV __xc_z[] = {nullptr};

// Pre-terminators (.CRT$XP*)
__declspec(allocate(".CRT$XPA")) __declspec(selectany) PVFV __xp_a[] = {nullptr};
__declspec(allocate(".CRT$XPZ")) __declspec(selectany) PVFV __xp_z[] = {nullptr};

// Terminators (.CRT$XT*)
__declspec(allocate(".CRT$XTA")) __declspec(selectany) PVFV __xt_a[] = {nullptr};
__declspec(allocate(".CRT$XTZ")) __declspec(selectany) PVFV __xt_z[] = {nullptr};
}

// Imported from c.dll — performs internal libc initialization.
extern "C" void __libc_init(int *argc, char ***argv, char ***env);

// Imported from c.dll — public API.
extern "C" int atexit(void (*)(void));
extern "C" [[noreturn]] void exit(int status);

static void call_init_array_callbacks(int argc, char **argv, char **env) {
  (void)argc;
  (void)argv;
  (void)env;

  // Run C initializers (.CRT$XI*) — non-zero return means failure.
  for (PIFV *fn = __xi_a; fn < __xi_z; ++fn) {
    if (*fn) {
      if ((*fn)() != 0)
        exit(255);
    }
  }

  // Run C++ constructors (.CRT$XC*)
  for (PVFV *fn = __xc_a; fn < __xc_z; ++fn) {
    if (*fn)
      (*fn)();
  }
}

static void call_fini_array_callbacks() {
  // Run pre-terminators (.CRT$XP*)
  for (PVFV *fn = __xp_a; fn < __xp_z; ++fn) {
    if (*fn)
      (*fn)();
  }

  // Run terminators (.CRT$XT*)
  for (PVFV *fn = __xt_a; fn < __xt_z; ++fn) {
    if (*fn)
      (*fn)();
  }
}

extern "C" [[noreturn]] void __libc_do_start() {
  // Security cookie must be initialized before any /GS-protected function.
  __security_init_cookie();

  // Cross-DLL data references in RTTI are handled by dynamic initialization
  // (.CRT$XIB) + section sealing (.CRT$XIY). Code-section references use
  // .refptr. stubs collapsed by the linker. No runtime pseudo-relocator needed.

  int argc;
  char **argv;
  char **env;
  __libc_init(&argc, &argv, &env);

  // Core subsystems (file_pool, fd_table, signal_state, std_fds) register
  // themselves in .CRT$XI{B,C,D,E} — the section walker runs them in letter
  // order before any user .CRT$XI{F..Y} or .CRT$XC* (C++ constructor) entries.
  atexit(&call_fini_array_callbacks);
  call_init_array_callbacks(argc, argv, env);

  int retval = call_main_guarded(argc, argv, env);
  exit(retval);
}
