//===-- crt_windows_init.cpp - CRT initialization -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// CRT initialization sections and routines.
//
// The linker merges .CRT$X* sections alphabetically:
//   .CRT$XIA -> .CRT$XIZ : C initializers (pre-init, return int)
//   .CRT$XCA -> .CRT$XCZ : C++ constructors (return void)
//   .CRT$XPA -> .CRT$XPZ : Pre-terminators
//   .CRT$XTA -> .CRT$XTZ : Terminators
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// CRT section declarations
//
// The linker merges .CRT$X* sections alphabetically within each group:
//   .CRT$XIA -> .CRT$XIZ : C initializers (pre-init, return int for errors)
//   .CRT$XCA -> .CRT$XCZ : C++ constructors (return void)
//   .CRT$XPA -> .CRT$XPZ : Pre-terminators (run before atexit handlers)
//   .CRT$XTA -> .CRT$XTZ : Terminators (run after atexit handlers)
//
// We define sentinel values at the start (A) and end (Z) of each section
// group. The linker places user-registered callbacks between these sentinels.
//===----------------------------------------------------------------------===//

// Initializer sections
#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)

// Terminator sections
#pragma section(".CRT$XPA", long, read)
#pragma section(".CRT$XPZ", long, read)
#pragma section(".CRT$XTA", long, read)
#pragma section(".CRT$XTZ", long, read)

extern "C" {

// C initializers (return int, non-zero indicates failure)
__declspec(allocate(".CRT$XIA")) _PIFV __xi_a[] = {nullptr};
__declspec(allocate(".CRT$XIZ")) _PIFV __xi_z[] = {nullptr};

// C++ constructors (return void)
__declspec(allocate(".CRT$XCA")) _PVFV __xc_a[] = {nullptr};
__declspec(allocate(".CRT$XCZ")) _PVFV __xc_z[] = {nullptr};

// Pre-terminators (run before atexit handlers, e.g., for flush operations)
__declspec(allocate(".CRT$XPA")) _PVFV __xp_a[] = {nullptr};
__declspec(allocate(".CRT$XPZ")) _PVFV __xp_z[] = {nullptr};

// Terminators (run after atexit handlers, e.g., for final cleanup)
__declspec(allocate(".CRT$XTA")) _PVFV __xt_a[] = {nullptr};
__declspec(allocate(".CRT$XTZ")) _PVFV __xt_z[] = {nullptr};

}

//===----------------------------------------------------------------------===//
// DSO handle
//
// __dso_handle is used by __cxa_atexit to identify which DSO (executable or
// DLL) registered an exit handler. The value is the address of the symbol
// itself, making it unique per DSO.
//
// We use __declspec(selectany) because:
// 1. Multiple translation units may include code referencing __dso_handle
// 2. Each DSO (exe/dll) that links this CRT object needs exactly one copy
// 3. selectany tells the linker to pick one definition and discard duplicates
//
// This is NOT an ODR violation - each DSO needs its own __dso_handle, and
// the linker handles this correctly per-module.
//===----------------------------------------------------------------------===//

extern "C" __declspec(selectany) void* __dso_handle = &__dso_handle;

//===----------------------------------------------------------------------===//
// Floating-point support
//
// The linker looks for _fltused when floating-point code is present.
// The value 0x9875 is the historical MSVC magic value.
//===----------------------------------------------------------------------===//

extern "C" __declspec(selectany) int _fltused = crt::FltusedMagic;

//===----------------------------------------------------------------------===//
// _initterm / _initterm_e implementation
//
// These iterate through arrays of function pointers placed in .CRT$X* sections
// by the compiler for static initialization.
//
// We export these as extern "C" symbols for compatibility with code that
// expects to call them directly (e.g., ASan runtime thunks, mixed-mode
// assemblies, custom loaders).
//===----------------------------------------------------------------------===//

// Internal implementations
namespace crt {

void initterm(_PVFV* first, _PVFV* last) {
  for (_PVFV* fn = first; fn < last; ++fn) {
    if (*fn)
      (*fn)();
  }
}

int initterm_e(_PIFV* first, _PIFV* last) {
  for (_PIFV* fn = first; fn < last; ++fn) {
    if (*fn) {
      int ret = (*fn)();
      if (ret)
        return ret;
    }
  }
  return 0;
}

} // namespace crt

// Public exports - these are the symbols that external code references
extern "C" {

void __cdecl _initterm(_PVFV* first, _PVFV* last) {
  crt::initterm(first, last);
}

int __cdecl _initterm_e(_PIFV* first, _PIFV* last) {
  return crt::initterm_e(first, last);
}

} // extern "C"

//===----------------------------------------------------------------------===//
// _CRT_INIT - DLL CRT initialization entry point
//
// This function can be called by custom loaders or mixed-mode assemblies
// that need explicit control over CRT initialization timing.
//
// Parameters:
//   hinstDLL  - Handle to the DLL module
//   fdwReason - Reason for calling (DLL_PROCESS_ATTACH, etc.)
//   lpReserved - Reserved; nullptr for dynamic loads, non-nullptr for static
//
// Returns:
//   TRUE on success, FALSE on failure
//
// Usage:
//   For DLLs that need custom entry points but still want CRT initialization,
//   call _CRT_INIT from your custom entry point:
//
//     BOOL WINAPI CustomDllEntry(HINSTANCE h, DWORD reason, LPVOID reserved) {
//       if (!_CRT_INIT(h, reason, reserved))
//         return FALSE;
//       // Your initialization code here
//       return TRUE;
//     }
//===----------------------------------------------------------------------===//

extern "C" BOOL __stdcall _CRT_INIT(HINSTANCE hinstDLL, DWORD fdwReason,
                                    LPVOID lpvReserved) {
  (void)hinstDLL;

  if (fdwReason == crt::kDllProcessAttach) {
    crt::securityInitCookie();

    // Run C initializers
    if (crt::initterm_e(__xi_a, __xi_z) != 0)
      return FALSE;

    // Run C++ constructors
    crt::initterm(__xc_a, __xc_z);
  } else if (fdwReason == crt::kDllProcessDetach) {
    // Run cleanup for programs that use _CRT_INIT directly (e.g., mixed-mode
    // assemblies, custom loaders). Programs using _DllMainCRTStartup get
    // cleanup through that path instead.
    //
    // lpvReserved: nullptr = FreeLibrary (explicit unload)
    //              non-nullptr = process terminating
    //
    // On explicit unload, run this DLL's cleanup.
    // On process termination, let exit() handle cleanup to ensure proper
    // ordering across all modules.
    if (lpvReserved == nullptr) {
      // Run C++ destructors for this module
      CRT_CXA_FINALIZE_CALL(__dso_handle);

      // Run pre-terminators and terminators
      crt::runPreterminators();
      crt::runTerminators();
    }
  }

  return TRUE;
}

//===----------------------------------------------------------------------===//
// Common initialization routine (internal)
//
// This function is guarded against double initialization. This can occur if:
// - Both an EXE and a DLL in the same process use this CRT
// - _CRT_INIT is called explicitly after implicit initialization
// - Unusual loader scenarios with manifest-based isolation
//
// The guard ensures C initializers and C++ constructors run exactly once
// per module. Note that the section-based initializer lists (__xi_a, etc.)
// are already per-module due to PE section semantics.
//===----------------------------------------------------------------------===//

namespace crt {

//===----------------------------------------------------------------------===//
// One-time initialization via InitOnceExecuteOnce
//
// We use the Windows-provided InitOnceExecuteOnce API instead of hand-rolled
// spin locks. Benefits:
// - Kernel-assisted waiting (no CPU burn while waiting)
// - Proper handling of all edge cases (priority inversion, etc.)
// - Well-tested (used throughout Windows)
// - Self-documenting intent
//===----------------------------------------------------------------------===//

namespace {

// Static initialization - no runtime init needed
INIT_ONCE g_commonInitOnce = INIT_ONCE_STATIC_INIT;

// Callback that performs the actual initialization
// Returns TRUE on success, FALSE to allow retry (we always succeed or abort)
BOOL __stdcall commonInitCallback(PINIT_ONCE, void*, void**) {
  // Process runtime pseudo-relocations FIRST, before anything else.
  // These fix up data symbol references that couldn't be resolved at link time
  // (e.g., extern data from DLLs when using -runtime-pseudo-reloc).
  // This must happen before securityInitCookie() and C/C++ initializers because
  // they might reference symbols that need relocation.
  runPseudoRelocator();

  securityInitCookie();

  // Initialize floating-point state to a known configuration.
  // This ensures consistent FPU behavior regardless of what state the loader
  // left it in. Matches MSVC CRT behavior.
  _fpreset();

  // Run C initializers (these can return error codes)
  if (initterm_e(__xi_a, __xi_z) != 0)
    fatalError(RuntimeError::CrtInit);

  // Run C++ constructors
  initterm(__xc_a, __xc_z);

  return TRUE;
}

} // namespace

void commonInit() {
  // InitOnceExecuteOnce guarantees:
  // - Callback runs exactly once, even with concurrent callers
  // - Waiting threads sleep efficiently (kernel event, not spin)
  // - Proper memory barriers on completion
  InitOnceExecuteOnce(&g_commonInitOnce, commonInitCallback, nullptr, nullptr);
}

//===----------------------------------------------------------------------===//
// Termination routines
//
// These run callbacks registered in the .CRT$XP* and .CRT$XT* sections.
// The shutdown sequence for exit() is:
//   1. __cxa_finalize(nullptr) - C++ static destructors (via libc++abi)
//   2. runPreterminators()     - .CRT$XP* callbacks (e.g., stdio flush)
//   3. runTerminators()        - .CRT$XT* callbacks (our CRT's terminators)
//   4. UCRT _exit()            - atexit handlers, stdio flush, ExitProcess
//
// We explicitly run our terminators rather than relying on UCRT because:
// - UCRT has its own .CRT$XT* section markers that don't see our callbacks
// - This ensures deterministic cleanup regardless of UCRT implementation
//===----------------------------------------------------------------------===//

void runPreterminators() {
  initterm(__xp_a, __xp_z);
}

void runTerminators() {
  initterm(__xt_a, __xt_z);
}

} // namespace crt

//===----------------------------------------------------------------------===//
// Public terminator exports
//
// These match MSVC's exports for compatibility with code that calls them
// directly (e.g., custom loaders, mixed-mode assemblies).
//===----------------------------------------------------------------------===//

extern "C" {

void __cdecl _cexit(void) {
  // Perform cleanup and return to caller (unlike exit() which terminates).
  // This matches MSVC behavior: run C++ destructors, pre-terminators,
  // atexit handlers, and terminators.
  //
  // Sequence:
  // 1. C++ static destructors via __cxa_finalize
  // 2. Pre-terminators (.CRT$XP*)
  // 3. Terminators (.CRT$XT*)
  //
  // Note: We don't call UCRT's atexit handlers here because _cexit()
  // should only run CRT-level cleanup. Programs using atexit() with UCRT
  // should call exit() for full cleanup.
  CRT_CXA_FINALIZE_CALL(nullptr);
  crt::runPreterminators();
  crt::runTerminators();
}

void __cdecl _c_exit(void) {
  // Minimal cleanup version - returns without running ANY cleanup.
  // This is for scenarios where:
  // - The process is about to terminate anyway
  // - Cleanup would cause issues (e.g., partially torn-down state)
  // - Performance is critical and cleanup is unnecessary
  //
  // WARNING: Static destructors and atexit handlers will NOT run.
}

} // extern "C"

//===----------------------------------------------------------------------===//
// exit() implementation
//
// Shutdown sequence:
// 1. __cxa_finalize(nullptr) - C++ static destructors (LIFO order)
// 2. Pre-terminators (.CRT$XP*) - early cleanup callbacks
// 3. Terminators (.CRT$XT*) - final cleanup callbacks
// 4. UCRT's _exit() - atexit handlers, stdio flush, ExitProcess
//
// We delegate atexit()/_onexit() to UCRT. C++ destructors go through
// __cxa_atexit (libc++abi). This means C++ destructors run before C atexit
// handlers, which matches typical mixed C/C++ Windows behavior.
//
// NOTE: We explicitly run our own terminators rather than relying on UCRT
// because UCRT uses its own .CRT$XT* section markers that may not match ours.
// This ensures callbacks registered via our CRT sections are always invoked.
//===----------------------------------------------------------------------===//

extern "C" CRT_NORETURN void __cdecl exit(int code) {
  // Run C++ static destructors registered via __cxa_atexit.
  // Passing nullptr runs ALL destructors, not just those for a specific DSO.
  CRT_CXA_FINALIZE_CALL(nullptr);

  // Run pre-terminators (.CRT$XP* section callbacks).
  // These run before atexit handlers, typically used for flush operations.
  crt::runPreterminators();

  // Run terminators (.CRT$XT* section callbacks).
  // We run these explicitly rather than relying on UCRT because UCRT's
  // section-based terminators may not see our CRT's registered callbacks.
  crt::runTerminators();

  // Delegate to UCRT for atexit handlers, stdio flush, and ExitProcess.
  _exit(code);
}

//===----------------------------------------------------------------------===//
// Pure virtual call handlers
//
// Provides MSVC ABI compatibility (_purecall) and Itanium ABI handlers
// (__cxa_pure_virtual, __cxa_deleted_virtual).
//
// Design: __cxa_pure_virtual (from libc++abi) is authoritative. The MSVC
// _purecall symbol delegates to it for SDK compatibility. When libc++abi
// is linked, its implementation is used; otherwise our weak fallback calls
// abort(). For customization with libc++abi, use std::set_terminate().
//
// We don't provide _set_purecall_handler because UCRT already has it and
// libc++abi bypasses it anyway (uses std::terminate instead).
//===----------------------------------------------------------------------===//

// Forward declaration for Itanium handler
extern "C" CRT_NORETURN void __cxa_pure_virtual(void);

//===----------------------------------------------------------------------===//
// __cxa_pure_virtual / __cxa_deleted_virtual - Itanium ABI handlers
//
// These are weak symbols that provide fallback behavior for pure C programs
// or programs that don't link libc++abi. When libc++abi is linked, its
// strong definitions override these.
//
// For Windows Itanium, these are the primary handlers since the compiler
// generates Itanium-style vtables.
//
// IMPORTANT: These must be defined BEFORE _purecall so the weak symbol
// resolution works correctly when _purecall calls __cxa_pure_virtual.
//===----------------------------------------------------------------------===//

extern "C" {

CRT_WEAK CRT_NORETURN void __cxa_pure_virtual(void) {
  // Use abort() from UCRT for proper crash dump generation.
  // This is the fallback when libc++abi is not linked.
  abort();
}

CRT_WEAK CRT_NORETURN void __cxa_deleted_virtual(void) {
  // Deleted virtual functions have the same termination behavior
  abort();
}

} // extern "C"

//===----------------------------------------------------------------------===//
// _purecall - MSVC ABI pure virtual handler
//
// This is referenced by vtables generated for classes with pure virtual
// functions when using the MSVC ABI. Windows SDK types and code compiled
// with /EHsc may reference this symbol.
//
// We wrap this to call __cxa_pure_virtual, ensuring that the Itanium handler
// (from libc++abi when linked) is always the authoritative handler. This
// gives us consistent behavior whether the pure virtual call came from
// MSVC-ABI code (Windows SDK) or Itanium-ABI code (our libc++ code).
//===----------------------------------------------------------------------===//

extern "C" {

int __cdecl _purecall(void) {
  // Delegate to Itanium handler - this will use libc++abi's implementation
  // if linked, otherwise falls back to our weak definition above.
  //
  // Note: MSVC's _purecall returns int for historical compatibility, even
  // though it never actually returns. We match this signature exactly.
  // The __cxa_pure_virtual call will terminate the process.
  __cxa_pure_virtual();

  // This point is never reached since __cxa_pure_virtual is noreturn.
  // The unreachable hint helps the optimizer eliminate dead code and
  // suppress "missing return" warnings without adding actual instructions.
  __builtin_unreachable();
}

} // extern "C"

#endif // _WIN32
