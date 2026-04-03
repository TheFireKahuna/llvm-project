// Test WinMain command line parsing.
//
// RUN: %clang_crt_winmain %s -o %t.exe
// RUN: %run %t.exe arg1 "arg two" | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <string.h>

// Windows types without windows.h.
typedef void *HINSTANCE;
typedef char *LPSTR;

#define SW_SHOWDEFAULT 10

int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, int nShowCmd) {
  (void)hInstance;
  (void)hPrevInstance;

  // CHECK: WinMain command line test
  printf("WinMain command line test\n");

  // CHECK: hInstance valid = 1
  printf("hInstance valid = %d\n", hInstance != 0);

  // CHECK: hPrevInstance = 0
  printf("hPrevInstance = %d\n", hPrevInstance == 0 ? 0 : 1);

  // lpCmdLine should contain arguments (without program name).
  // CHECK: lpCmdLine not null = 1
  printf("lpCmdLine not null = %d\n", lpCmdLine != NULL);

  // CHECK: lpCmdLine = arg1 "arg two"
  printf("lpCmdLine = %s\n", lpCmdLine);

  // CHECK: nShowCmd valid = 1
  printf("nShowCmd valid = %d\n", nShowCmd >= 0);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
