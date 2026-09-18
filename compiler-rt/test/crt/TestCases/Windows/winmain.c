// Test WinMain() GUI entry point.
//
// RUN: %clang_crt_winmain %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

typedef void *HINSTANCE;
typedef char *LPSTR;
typedef int BOOL;

#define __stdcall __attribute__((ms_abi))

#include <stdio.h>

int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, int nCmdShow) {
  // CHECK: WinMain() called
  printf("WinMain() called\n");

  // CHECK: hInstance exists = 1
  printf("hInstance exists = %d\n", hInstance != 0);

  // Always NULL on Win32.
  // CHECK: hPrevInstance = 0
  printf("hPrevInstance = %d\n", hPrevInstance != 0);

  // CHECK: lpCmdLine exists = 1
  printf("lpCmdLine exists = %d\n", lpCmdLine != 0);

  // CHECK: nCmdShow valid = 1
  printf("nCmdShow valid = %d\n", nCmdShow >= 0);

  // CHECK: exit code = 0
  printf("exit code = 0\n");
  return 0;
}
