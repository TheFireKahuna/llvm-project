// RUN: %clang_wincrt %s -lm -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// -lm links: the math functions come from the Universal CRT, and the math
// library that POSIX build systems name is empty.

#include <math.h>
#include <stdio.h>

int main(int argc, char **argv) {
  (void)argv;
  printf("%g\n", sqrt(16.0 * argc));
  return 0;
}

// CHECK: 4
