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

// Per-module DSO handle — set to HINSTANCE on DLL_PROCESS_ATTACH.
// __cxa_atexit stores this so __cxa_finalize can run only this DLL's dtors.
extern "C" void *__dso_handle;
__declspec(selectany) void *__dso_handle = nullptr;

// Itanium ABI cleanup. __cxa_finalize provided by atexit.cpp,
// __cxa_thread_finalize_dso_unload by thread.cpp —
// both compiled alongside this file via dll_crt.cpp.
extern "C" void __cxa_finalize(void *dso);
extern "C" void __cxa_thread_finalize_dso_unload(void *dso);

// Security cookie init — must run before any /GS-protected function.
extern "C" void __security_init_cookie(void);

// CRT section function pointer types.
using PIFV = int(__cdecl *)(void);   /* C init — returns 0 on success */
using PVFV = void(__cdecl *)(void);  /* C++ ctor / dtor */

// CRT section sentinel pairs — linker merges .CRT$X?A through .CRT$X?Z.
// Subsystems place their init/fini pointers between A and Z via section
// letter (e.g. .CRT$XIB for file_pool, .CRT$XIC for fd_table).
#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
#pragma section(".CRT$XPA", long, read)
#pragma section(".CRT$XPZ", long, read)
#pragma section(".CRT$XTA", long, read)
#pragma section(".CRT$XTZ", long, read)

extern "C" {
// C initializers (.CRT$XI*)
__declspec(allocate(".CRT$XIA")) __declspec(selectany) PIFV __xi_a_dll[] = {
    nullptr};
__declspec(allocate(".CRT$XIZ")) __declspec(selectany) PIFV __xi_z_dll[] = {
    nullptr};

// C++ constructors (.CRT$XC*)
__declspec(allocate(".CRT$XCA")) __declspec(selectany) PVFV __xc_a_dll[] = {
    nullptr};
__declspec(allocate(".CRT$XCZ")) __declspec(selectany) PVFV __xc_z_dll[] = {
    nullptr};

// Pre-terminators (.CRT$XP*)
__declspec(allocate(".CRT$XPA")) __declspec(selectany) PVFV __xp_a_dll[] = {
    nullptr};
__declspec(allocate(".CRT$XPZ")) __declspec(selectany) PVFV __xp_z_dll[] = {
    nullptr};

// Terminators (.CRT$XT*)
__declspec(allocate(".CRT$XTA")) __declspec(selectany) PVFV __xt_a_dll[] = {
    nullptr};
__declspec(allocate(".CRT$XTZ")) __declspec(selectany) PVFV __xt_z_dll[] = {
    nullptr};

}

namespace {

int run_c_init() {
  for (PIFV *fn = __xi_a_dll; fn < __xi_z_dll; ++fn) {
    if (*fn) {
      int result = (*fn)();
      if (result != 0)
        return result;
    }
  }
  return 0;
}

void run_cpp_init() {
  for (PVFV *fn = __xc_a_dll; fn < __xc_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

void run_preterminators() {
  for (PVFV *fn = __xp_a_dll; fn < __xp_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

void run_terminators() {
  for (PVFV *fn = __xt_a_dll; fn < __xt_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

} // namespace

// Default DllMain for DLLs that don't define one (runtime libs, etc.).
// User code overrides via linker — if DllMain is defined, theirs wins.
extern "C" int /* BOOL */ __stdcall
__libc_DefaultDllMain(void * /* HINSTANCE */, unsigned long /* DWORD */,
                      void * /* LPVOID */) {
  return 1 /* TRUE */;
}

#if defined(__i386__)
#pragma comment(                                                               \
    linker, "/alternatename:_DllMain@12=___libc_DefaultDllMain@12")
#else
#pragma comment(                                                               \
    linker, "/alternatename:DllMain=__libc_DefaultDllMain")
#endif

extern "C" int /* BOOL */ __stdcall
DllMain(void * /* HINSTANCE */, unsigned long /* DWORD */,
        void * /* LPVOID */);

extern "C" __declspec(dllexport) int /* BOOL */ __stdcall
_DllMainCRTStartup(void *hinstDLL,          /* HINSTANCE */
                   unsigned long fdwReason,  /* DWORD */
                   void *lpvReserved) {      /* LPVOID */
  if (fdwReason == 1 /* DLL_PROCESS_ATTACH */) {
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

  if (fdwReason == 0 /* DLL_PROCESS_DETACH */) {
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
