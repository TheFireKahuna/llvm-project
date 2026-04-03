// Test basic main() entry point.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %run %t.exe arg1 arg2 | FileCheck %s --check-prefix=CHECK-ARGS
//
// REQUIRES: windows, crt

#include <stdio.h>

int main(int argc, char **argv, char **envp) {
  // CHECK: main() called
  // CHECK-ARGS: main() called
  printf("main() called\n");

  // CHECK: argc = 1
  // CHECK-ARGS: argc = 3
  printf("argc = %d\n", argc);

  // CHECK: argv[0] exists = 1
  // CHECK-ARGS: argv[0] exists = 1
  printf("argv[0] exists = %d\n", argv[0] != NULL);

  // CHECK: argv null-terminated = 1
  // CHECK-ARGS: argv null-terminated = 1
  printf("argv null-terminated = %d\n", argv[argc] == NULL);

  // CHECK: envp exists = 1
  // CHECK-ARGS: envp exists = 1
  printf("envp exists = %d\n", envp != NULL);

  // CHECK-ARGS: argv[1] = arg1
  // CHECK-ARGS: argv[2] = arg2
  for (int i = 1; i < argc; i++) {
    printf("argv[%d] = %s\n", i, argv[i]);
  }

  // CHECK: exit code = 0
  // CHECK-ARGS: exit code = 0
  printf("exit code = 0\n");
  return 0;
}
