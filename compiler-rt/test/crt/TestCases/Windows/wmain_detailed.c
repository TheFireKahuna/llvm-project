// Test wide-character main (wmain) with arguments.
//
// RUN: %clang_crt_wmain %s -o %t.exe
// RUN: %run %t.exe arg1 "wide arg" 测试 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <wchar.h>

// UCRT wide argument accessors.
extern wchar_t ***__p___wargv(void);
extern int *__p___argc(void);

int wmain(int argc, wchar_t **argv) {
  // CHECK: Wide main test
  printf("Wide main test\n");

  // CHECK: argc = 4
  printf("argc = %d\n", argc);

  // Verify __wargv matches.
  wchar_t **global_wargv = *__p___wargv();
  // CHECK: wargv matches = 1
  printf("wargv matches = %d\n", argv == global_wargv);

  // Print arguments (narrow for portability).
  for (int i = 0; i < argc; ++i) {
    printf("argv[%d] = %ls\n", i, argv[i]);
  }

  // CHECK: argv[1] = arg1
  // CHECK: argv[2] = wide arg

  // Verify null termination.
  // CHECK: null terminated = 1
  printf("null terminated = %d\n", argv[argc] == NULL);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
