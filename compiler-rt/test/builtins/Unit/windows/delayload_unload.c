// RUN: %clang_wincrt -shared -DBUILD_DLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll -Wl,/delay:unload
// RUN: %run %t.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=UNLOAD
// RUN: %clang_wincrt -mguard=cf %s %t.dll.lib -o %t-protected.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll -Wl,/delay:unload
// RUN: %run %t-protected.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=UNLOAD
// RUN: %clang_wincrt %s %t.dll.lib -o %t-no-unload.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t-no-unload.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NO-UNLOAD

// __FUnloadDelayLoadedDLL2 restores the address table and frees the library,
// so that the next call loads it again. It needs the copy of the table that
// /delay:unload puts in the image, and refuses without it. It also refuses a
// library the image does not delay-load, and matches the name without regard
// to case.

#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef BUILD_DLL
__declspec(dllexport) int twice(int Value) { return 2 * Value; }
#else

__declspec(dllimport) int twice(int);
BOOL WINAPI __FUnloadDelayLoadedDLL2(LPCSTR);

int main(int argc, char **argv) {
  if (argc != 2 || GetModuleHandleA(argv[1]) || twice(21) != 42 ||
      !GetModuleHandleA(argv[1]))
    return 1;
  if (__FUnloadDelayLoadedDLL2("no-such-library.dll"))
    return 2;

  char Name[MAX_PATH];
  strcpy(Name, argv[1]);
  CharUpperA(Name);
  BOOL Unloaded = __FUnloadDelayLoadedDLL2(Name);
  fprintf(stderr, "unloaded %d, still loaded %d\n", Unloaded,
          GetModuleHandleA(argv[1]) != NULL);
  if (!Unloaded)
    return 0;
  if (__FUnloadDelayLoadedDLL2(argv[1]))
    return 3;

  // The table holds the thunks again, so the next call loads the DLL afresh.
  fprintf(stderr, "twice(4) = %d, loaded %d\n", twice(4),
          GetModuleHandleA(argv[1]) != NULL);
  return 0;
}
#endif

// UNLOAD: unloaded 1, still loaded 0
// UNLOAD-NEXT: twice(4) = 8, loaded 1

// NO-UNLOAD: unloaded 0, still loaded 1
// NO-UNLOAD-NOT: twice
