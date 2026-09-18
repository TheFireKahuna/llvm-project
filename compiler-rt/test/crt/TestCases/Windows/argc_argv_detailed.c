// Test detailed argument parsing and access.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe arg1 "arg with spaces" 'single quoted' --flag=value | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <string.h>

// UCRT argument accessors.
extern int *__p___argc(void);
extern char ***__p___argv(void);

int main(int argc, char **argv) {
  // CHECK: Argument parsing test
  printf("Argument parsing test\n");

  // argc should match parameter.
  int global_argc = *__p___argc();
  // CHECK: argc matches = 1
  printf("argc matches = %d\n", argc == global_argc);

  // argv should match parameter.
  char **global_argv = *__p___argv();
  // CHECK: argv matches = 1
  printf("argv matches = %d\n", argv == global_argv);

  // CHECK: argc = 5
  printf("argc = %d\n", argc);

  // CHECK: argv[0] exists = 1
  printf("argv[0] exists = %d\n", argv[0] != NULL && strlen(argv[0]) > 0);

  // CHECK: argv[1] = arg1
  printf("argv[1] = %s\n", argc > 1 ? argv[1] : "(missing)");

  // CHECK: argv[2] = arg with spaces
  printf("argv[2] = %s\n", argc > 2 ? argv[2] : "(missing)");

  // CHECK: argv[3] = single quoted
  printf("argv[3] = %s\n", argc > 3 ? argv[3] : "(missing)");

  // CHECK: argv[4] = --flag=value
  printf("argv[4] = %s\n", argc > 4 ? argv[4] : "(missing)");

  // argv should be null-terminated.
  // CHECK: argv null-terminated = 1
  printf("argv null-terminated = %d\n", argv[argc] == NULL);

  // Test __argc / __argv macros (via accessors).
  // CHECK: global argc = 5
  printf("global argc = %d\n", *__p___argc());

  // Verify no buffer overruns.
  int valid_args = 1;
  for (int i = 0; i < argc; ++i) {
    if (argv[i] == NULL) {
      valid_args = 0;
      break;
    }
  }
  // CHECK: all args valid = 1
  printf("all args valid = %d\n", valid_args);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
