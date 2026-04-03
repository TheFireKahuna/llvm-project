// Test command line argument parsing and CRT globals.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s --check-prefix=CHECK-NOARGS
// RUN: %run %t.exe one two three | FileCheck %s --check-prefix=CHECK-ARGS
// RUN: %run %t.exe "arg with spaces" | FileCheck %s --check-prefix=CHECK-SPACES
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <string.h>

extern int __argc;
extern char **__argv;

int main(int argc, char **argv) {
  printf("argc = %d\n", argc);
  printf("__argc = %d\n", __argc);
  printf("argc matches = %d\n", argc == __argc);

  // CHECK-NOARGS: argc = 1
  // CHECK-NOARGS: __argc = 1
  // CHECK-NOARGS: argc matches = 1

  // CHECK-ARGS: argc = 4
  // CHECK-ARGS: __argc = 4
  // CHECK-ARGS: argc matches = 1

  // CHECK-SPACES: argc = 2
  // CHECK-SPACES: __argc = 2
  // CHECK-SPACES: argc matches = 1

  printf("argv matches = %d\n", argv == __argv);
  // CHECK-NOARGS: argv matches = 1
  // CHECK-ARGS: argv matches = 1
  // CHECK-SPACES: argv matches = 1

  for (int i = 0; i < argc; i++) {
    printf("argv[%d] = \"%s\"\n", i, argv[i]);
  }

  // CHECK-ARGS: argv[1] = "one"
  // CHECK-ARGS: argv[2] = "two"
  // CHECK-ARGS: argv[3] = "three"

  // CHECK-SPACES: argv[1] = "arg with spaces"

  printf("argv[argc] is NULL = %d\n", argv[argc] == NULL);
  // CHECK-NOARGS: argv[argc] is NULL = 1
  // CHECK-ARGS: argv[argc] is NULL = 1
  // CHECK-SPACES: argv[argc] is NULL = 1

  return 0;
}
