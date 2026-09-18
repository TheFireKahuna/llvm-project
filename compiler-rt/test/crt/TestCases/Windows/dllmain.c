// Test DllMain() entry point. Builds a DLL and loader to verify callbacks.
//
// RUN: %clang_crt_dll %s -o %t.dll
// RUN: llvm-readobj --coff-imports --coff-exports %t.dll | FileCheck %s --check-prefix=SYMBOLS
// RUN: %clang_crt_main %S/Inputs/dll_loader.c -o %t_loader.exe
// RUN: %run %t_loader.exe %t.dll | FileCheck %s
//
// REQUIRES: windows, crt

typedef void *HINSTANCE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef int BOOL;

#define __stdcall __attribute__((ms_abi))
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
#define TRUE 1
#define FALSE 0

#include <stdio.h>

BOOL __stdcall DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
  case DLL_PROCESS_ATTACH:
    printf("DllMain: DLL_PROCESS_ATTACH\n");
    printf("hinstDLL exists = %d\n", hinstDLL != 0);
    break;

  case DLL_PROCESS_DETACH:
    printf("DllMain: DLL_PROCESS_DETACH\n");
    break;

  case DLL_THREAD_ATTACH:
    printf("DllMain: DLL_THREAD_ATTACH\n");
    break;

  case DLL_THREAD_DETACH:
    printf("DllMain: DLL_THREAD_DETACH\n");
    break;
  }

  return TRUE;
}

__declspec(dllexport) int test_function(void) {
  printf("test_function() called\n");
  return 42;
}

// CHECK: DllMain: DLL_PROCESS_ATTACH
// CHECK: hinstDLL exists = 1
// CHECK: test_function() called
// CHECK: test_function returned: 42
// CHECK: DllMain: DLL_PROCESS_DETACH
// CHECK: Done
// SYMBOLS-NOT: _DllMainCRTStartup
