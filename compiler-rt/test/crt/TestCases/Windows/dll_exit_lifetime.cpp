// Destruction order must span EXE and DLL registrations at process exit.
// RUN: %clangxx_crt_dll -O2 %S/Inputs/lifetime_module.cpp -o %t.first.dll
// RUN: %clangxx_crt_dll -O2 %S/Inputs/lifetime_module.cpp -o %t.second.dll
// RUN: %clangxx_crt_main -O2 %s -o %t.exe
// RUN: %run %t.exe %t.first.dll %t.second.dll unload 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t.exe %t.first.dll %t.second.dll immediate 2>&1 | FileCheck %s --check-prefix=IMMEDIATE
// RUN: %run %t.exe %t.first.dll %t.second.dll return 2>&1 | FileCheck %s --check-prefix=NORMAL
// RUN: %run %t.exe %t.first.dll %t.second.dll exit 2>&1 | FileCheck %s --check-prefix=NORMAL
// REQUIRES: windows, crt

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

struct Object {
  int id;
  explicit Object(int value) : id(value) {
    fprintf(stderr, "construct %d\n", id);
  }
  ~Object() { fprintf(stderr, "destroy %d\n", id); }
};

static void onExit() { fprintf(stderr, "atexit\n"); }

int main(int argc, char **argv) {
  assert(argc == 4);
  HMODULE first = LoadLibraryA(argv[1]);
  HMODULE second = LoadLibraryA(argv[2]);
  assert(first && second);
  using Construct = void (*)(int);
  auto constructFirst =
      reinterpret_cast<Construct>(GetProcAddress(first, "construct"));
  auto constructSecond =
      reinterpret_cast<Construct>(GetProcAddress(second, "construct"));
  assert(constructFirst && constructSecond);

  static Object one(1);
  constructFirst(2);
  static Object three(3);
  constructSecond(4);
  static Object five(5);
  assert(atexit(onExit) == 0);

  if (argv[3][0] == 'u') {
    assert(FreeLibrary(second));
    assert(FreeLibrary(first));
  }
  fprintf(stderr, "leaving main\n");
  if (argv[3][0] == 'i')
    _Exit(0);
  if (argv[3][0] == 'e')
    exit(0);
  return 0;
}

// NORMAL: leaving main
// NORMAL-NEXT: atexit
// NORMAL-NEXT: destroy 5
// NORMAL-NEXT: destroy 4
// NORMAL-NEXT: destroy 3
// NORMAL-NEXT: destroy 2
// NORMAL-NEXT: destroy 1
// NORMAL-NOT: destroy

// UNLOAD: destroy 4
// UNLOAD-NEXT: destroy 2
// UNLOAD-NEXT: leaving main
// UNLOAD-NEXT: atexit
// UNLOAD-NEXT: destroy 5
// UNLOAD-NEXT: destroy 3
// UNLOAD-NEXT: destroy 1
// UNLOAD-NOT: destroy

// IMMEDIATE: leaving main
// IMMEDIATE-NOT: atexit
// IMMEDIATE-NOT: destroy
