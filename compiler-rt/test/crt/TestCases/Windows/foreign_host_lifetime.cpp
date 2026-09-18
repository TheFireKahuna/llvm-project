// An Itanium C++ DLL inside a host that does not use the shared C++ runtime
// finalizes its own registrations at process detach, as vcruntime DLLs do. A
// C-only wincrt executable never registers with c++.dll's registry, so it
// stands in for an MSVC or non-CRT host here.
// RUN: %clangxx_crt_dll -std=c++17 -O2 -DBUILD_DLL %s -o %t.dll
// RUN: %clang_crt_main -x c -O2 -UNDEBUG -DBUILD_C_HOST %s -o %t.exe
// RUN: %run %t.exe %t.dll exit 2>&1 | FileCheck %s --check-prefix=EXIT
// RUN: %run %t.exe %t.dll unload 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t.exe %t.dll immediate 2>&1 | FileCheck %s --check-prefix=IMMEDIATE
// RUN: %run %t.exe %t.dll quick 2>&1 | FileCheck %s --check-prefix=QUICK
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(BUILD_DLL)
struct Object {
  const char *name;
  ~Object() { fprintf(stderr, "destroy %s\n", name); }
};
static void normal() { fprintf(stderr, "atexit\n"); }
static void quick() { fprintf(stderr, "at_quick_exit\n"); }
extern "C" __declspec(dllexport) int touch() {
  static Object object{"static"};
  thread_local Object local{"thread_local"};
  return atexit(normal) || at_quick_exit(quick);
}
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void *) {
  if (reason == DLL_PROCESS_DETACH)
    fprintf(stderr, "detach\n");
  return TRUE;
}
#else
#  include <assert.h>

int main(int argc, char **argv) {
  assert(argc == 3);
  HMODULE module = LoadLibraryA(argv[1]);
  assert(module);
  int (*touch)(void) = (int (*)(void))GetProcAddress(module, "touch");
  assert(touch && touch() == 0);
  fprintf(stderr, "leaving\n");
  switch (argv[2][0]) {
  case 'u':
    assert(FreeLibrary(module));
    // This thread's pending thread_local keeps the image loaded until the
    // thread completes, which for a foreign host's main thread is process
    // exit; the registrations then run at that detach.
    assert(GetModuleHandleA(argv[1]));
    fprintf(stderr, "released\n");
    return 0;
  case 'i':
    _Exit(0);
  case 'q':
    quick_exit(0);
  default:
    exit(0);
  }
}
#endif

// Normal exit: the registry's UCRT token drains the DLL inside exit(), with
// the process still intact, before the loader detaches it: the thread-local
// first, then the static registrations in reverse order.
// EXIT: leaving
// EXIT-NEXT: destroy thread_local
// EXIT-NEXT: atexit
// EXIT-NEXT: destroy static
// EXIT-NEXT: detach
// EXIT-NOT: {{destroy|at_quick_exit}}

// The host released its handle, so the pending thread-local is the last
// reference: draining it inside exit() unloads the DLL at once, and its
// remaining registrations run through that detach.
// UNLOAD: leaving
// UNLOAD-NEXT: released
// UNLOAD-NEXT: destroy thread_local
// UNLOAD-NEXT: detach
// UNLOAD-NEXT: atexit
// UNLOAD-NEXT: destroy static
// UNLOAD-NOT: {{destroy|at_quick_exit}}

// _Exit runs no exit processing, so nothing runs before the loader's detach.
// A foreign host gives the DLL no way to distinguish that from exit, so its
// registrations still run at detach, matching vcruntime. Only a wincrt C++
// executable suppresses them (dll_exit_lifetime.cpp, IMMEDIATE).
// IMMEDIATE: leaving
// IMMEDIATE-NEXT: detach
// IMMEDIATE-NEXT: destroy thread_local
// IMMEDIATE-NEXT: atexit
// IMMEDIATE-NEXT: destroy static
// IMMEDIATE-NOT: {{destroy|at_quick_exit}}

// quick_exit runs the quick registrations through UCRT's table; the DLL's
// other registrations then follow at detach.
// QUICK: leaving
// QUICK-NEXT: at_quick_exit
// QUICK-NEXT: detach
// QUICK-NEXT: destroy thread_local
// QUICK-NEXT: atexit
// QUICK-NEXT: destroy static
// QUICK-NOT: {{destroy|at_quick_exit}}
