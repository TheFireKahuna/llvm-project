// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll exit | FileCheck %s --check-prefix=EXIT
// RUN: %run %t.exe %t.dll quick_exit | FileCheck %s --check-prefix=QUICK
// RUN: %run %t.exe %t.dll _Exit | FileCheck %s --check-prefix=NONE \
// RUN:     --allow-empty
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT
// RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=IMPORT

// Every image, a C DLL included, registers with the process's registries,
// which it imports from clang_rt.wincrt_dynamic.dll. A DLL's atexit
// functions then run at exit in one reverse order with the executable's, and
// its at_quick_exit functions run on quick_exit; _Exit runs neither, and
// neither does the DLL's detach when the process ends.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

static void dllAtexit(void) { printf("dll atexit\n"); }
static void dllQuick(void) {
  printf("dll at_quick_exit\n");
  fflush(stdout);
}

__declspec(dllexport) void registerInDll(void) {
  atexit(dllAtexit);
  at_quick_exit(dllQuick);
}

#else

static void exe1(void) { printf("exe atexit 1\n"); }
static void exe2(void) { printf("exe atexit 2\n"); }
static void exeQuick(void) {
  printf("exe at_quick_exit\n");
  fflush(stdout);
}

int main(int argc, char **argv) {
  atexit(exe1);
  at_quick_exit(exeQuick);
  HMODULE Module = LoadLibraryA(argv[1]);
  void (*Register)(void) =
      (void (*)(void))GetProcAddress(Module, "registerInDll");
  Register();
  atexit(exe2);
  fflush(stdout);
  if (!strcmp(argv[2], "quick_exit"))
    quick_exit(0);
  if (!strcmp(argv[2], "_Exit"))
    _Exit(0);
  return 0;
}

#endif

// EXIT:      exe atexit 2
// EXIT-NEXT: dll atexit
// EXIT-NEXT: exe atexit 1
// EXIT-NOT:  {{.}}

// QUICK:      dll at_quick_exit
// QUICK-NEXT: exe at_quick_exit
// QUICK-NOT:  {{.}}

// NONE-NOT: {{.}}

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
