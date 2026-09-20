// __FUnloadDelayLoadedDLL2 restores the delay-load address table and frees the
// library, so that a later call loads it again. It needs the copy of the table
// that /delay:unload puts in the image, and refuses without it.
// RUN: %clangxx_crt_dll -O2 -DBUILD_DLL %s -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -O2 %s %t.lib -o %t.exe -Wl,/delayload:%basename_t.tmp.dll -Wl,/delay:unload
// RUN: %run %t.exe %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %clangxx_crt_main -O2 %s %t.lib -o %t.no.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t.no.exe %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=NOUNLOAD
// REQUIRES: windows, crt

#include <stdio.h>

#if defined(BUILD_DLL)
extern "C" __declspec(dllexport) int delayed(int value) { return value * 2; }
#else
#  define WIN32_LEAN_AND_MEAN
#  include <assert.h>
#  include <windows.h>

extern "C" __declspec(dllimport) int delayed(int);
extern "C" BOOL __stdcall __FUnloadDelayLoadedDLL2(LPCSTR);

int main(int argc, char **argv) {
  assert(argc == 2);
  assert(!GetModuleHandleA(argv[1]));
  assert(delayed(21) == 42);
  assert(GetModuleHandleA(argv[1]));

  // A DLL this image does not delay-load is refused whatever it was linked
  // with, and the name is matched without regard to case.
  assert(!__FUnloadDelayLoadedDLL2("no-such-module.dll"));

  BOOL unloaded = __FUnloadDelayLoadedDLL2(argv[1]);
  fprintf(stderr, "unloaded %d, still loaded %d\n", unloaded,
          GetModuleHandleA(argv[1]) != nullptr);
  if (!unloaded)
    return 0;

  // The table holds the stubs again, so the next call loads the DLL afresh.
  assert(delayed(4) == 8);
  assert(GetModuleHandleA(argv[1]));
  fprintf(stderr, "reloaded\n");
  return 0;
}
#endif

// UNLOAD: unloaded 1, still loaded 0
// UNLOAD-NEXT: reloaded

// NOUNLOAD: unloaded 0, still loaded 1
// NOUNLOAD-NOT: reloaded
