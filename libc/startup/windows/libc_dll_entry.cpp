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

// Per-module DSO handle — set to HINSTANCE on DLL_PROCESS_ATTACH.
// __cxa_atexit stores this so __cxa_finalize can run only this DLL's dtors.
extern "C" void *__dso_handle;
__declspec(selectany) void *__dso_handle = nullptr;

// Itanium ABI cleanup. __cxa_finalize provided by atexit.cpp,
// __cxa_thread_finalize_dso_unload by the thread finalize implementation —
// both compiled into c.dll via WHOLEARCHIVE.
extern "C" void __cxa_finalize(void *dso);
extern "C" void __cxa_thread_finalize_dso_unload(void *dso);

// Security cookie init — must run before any /GS-protected function.
extern "C" void __security_init_cookie(void);

// Explicit subsystem init/fini — defined in libc_subsystem_init.cpp
// and libc_subsystem_fini.cpp, compiled into c.dll.
extern "C" int __libc_dll_init();
extern "C" void __libc_dll_fini();

// CRT section function pointer types.
using PVFV = void(__cdecl *)(void); // C++ constructors/destructors

// .CRT$XC* section sentinels — C++ constructors in c.dll.
// The linker merges .CRT$XCA through .CRT$XCZ alphabetically.
// We still walk these to support any C++ globals in c.dll.
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)

extern "C" {
__declspec(allocate(".CRT$XCA")) __declspec(selectany) PVFV __xc_a_dll[] = {
    nullptr};
__declspec(allocate(".CRT$XCZ")) __declspec(selectany) PVFV __xc_z_dll[] = {
    nullptr};
}

namespace {

void run_cpp_ctors() {
  for (PVFV *fn = __xc_a_dll; fn < __xc_z_dll; ++fn) {
    if (*fn)
      (*fn)();
  }
}

} // namespace

// Default DllMain for c.dll — returns TRUE. User code never defines
// DllMain in c.dll (it's a libc runtime, not user code).
extern "C" int /* BOOL */ __stdcall
__libc_DefaultDllMain(void * /* HINSTANCE */, unsigned long /* DWORD */,
                      void * /* LPVOID */) {
  return 1 /* TRUE */;
}

#if defined(__i386__)
#pragma comment(                                                               \
    linker, "/alternatename:_DllMain@12=___libc_DefaultDllMain@12")
#else
#pragma comment(linker, "/alternatename:DllMain=__libc_DefaultDllMain")
#endif

extern "C" int /* BOOL */ __stdcall DllMain(void * /* HINSTANCE */,
                                            unsigned long /* DWORD */,
                                            void * /* LPVOID */);

extern "C" __declspec(dllexport) int /* BOOL */ __stdcall
_DllMainCRTStartup(void *hinstDLL,          /* HINSTANCE */
                   unsigned long fdwReason,  /* DWORD */
                   void *lpvReserved) {      /* LPVOID */
  if (fdwReason == 1 /* DLL_PROCESS_ATTACH */) {
    __dso_handle = hinstDLL;

    // Security cookie must be initialized before any /GS-protected function.
    // Also initialized by the .CRT$XLAA TLS callback as defense-in-depth,
    // but the TLS callback might not fire if crt_tls.obj is somehow missing.
    __security_init_cookie();

    // Explicit subsystem initialization — no .CRT$XI* section walking.
    // All ordering is source-visible in libc_subsystem_init.cpp.
    if (__libc_dll_init() != 0)
      return 0 /* FALSE */;

    // C++ constructors (.CRT$XC*) — kept for any C++ globals in c.dll.
    run_cpp_ctors();

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

      // Explicit subsystem shutdown — no .CRT$XP* section walking.
      __libc_dll_fini();
    }
  }

  return DllMain(hinstDLL, fdwReason, lpvReserved);
}
