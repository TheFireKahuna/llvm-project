// Test wmain() entry point (wide character version).
//
// RUN: %clang_crt_wmain %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv, wchar_t **envp) {
  // CHECK: wmain() called
  printf("wmain() called\n");

  // CHECK: argc = 1
  printf("argc = %d\n", argc);

  // CHECK: argv[0] exists = 1
  printf("argv[0] exists = %d\n", argv[0] != NULL);

  // CHECK: argv[0] non-empty = 1
  printf("argv[0] non-empty = %d\n", argv[0][0] != L'\0');

  // CHECK: argv null-terminated = 1
  printf("argv null-terminated = %d\n", argv[argc] == NULL);

  // CHECK: envp exists = 1
  printf("envp exists = %d\n", envp != NULL);

  // CHECK: exit code = 0
  printf("exit code = 0\n");
  return 0;
}
