// Test static destructor cleanup on DLL unload.
//
// This test creates a DLL with static objects, loads it, then unloads it.
// Static destructors should run during FreeLibrary (DLL_PROCESS_DETACH).
//
// RUN: %clang_crt_dll %s -o %t.dll
// RUN: %clang_crt_main %S/Inputs/dll_unload_test.c -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s
//
// REQUIRES: windows, crt
// XFAIL: *

#include <stdio.h>

// Windows type definitions without windows.h.
typedef void *HINSTANCE;
typedef void *LPVOID;
typedef unsigned long DWORD;
typedef int BOOL;

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define TRUE 1
#define FALSE 0

#define __stdcall __attribute__((ms_abi))

struct DllStaticObject {
  int id;
  DllStaticObject(int i) : id(i) {
    printf("DllStaticObject(%d) constructed\n", id);
  }
  ~DllStaticObject() {
    printf("DllStaticObject(%d) destructed\n", id);
  }
};

static DllStaticObject g_obj1(1);
static DllStaticObject g_obj2(2);

extern "C" {

BOOL __stdcall DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;

  switch (fdwReason) {
  case DLL_PROCESS_ATTACH:
    printf("DllMain: PROCESS_ATTACH\n");
    break;
  case DLL_PROCESS_DETACH:
    printf("DllMain: PROCESS_DETACH\n");
    break;
  }
  return TRUE;
}

__declspec(dllexport) int dll_function(void) {
  printf("dll_function called, g_obj1.id=%d, g_obj2.id=%d\n",
         g_obj1.id, g_obj2.id);
  return g_obj1.id + g_obj2.id;
}

}

// CHECK: DllStaticObject(1) constructed
// CHECK: DllStaticObject(2) constructed
// CHECK: DllMain: PROCESS_ATTACH
// CHECK: dll_function called
// CHECK: DllMain: PROCESS_DETACH
// CHECK: DllStaticObject(2) destructed
// CHECK: DllStaticObject(1) destructed
