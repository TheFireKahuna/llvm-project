// RUN: %clang_wincrt %s -o %t.exe
// RUN: not %run %t.exe | FileCheck %s
// RUN: %python -c "import subprocess, sys; sys.exit(subprocess.call([sys.argv[1]], stdout=subprocess.DEVNULL) != 7)" %t.exe

// exit called by an atexit function neither waits for the exit already under
// way nor runs any function twice, and ends the process with its own code.

#include <stdio.h>
#include <stdlib.h>

static void first(void) { printf("first\n"); }
static void reenter(void) {
  printf("reenter\n");
  exit(7);
}
static void last(void) { printf("last\n"); }

int main(void) {
  atexit(first);
  atexit(reenter);
  atexit(last);
  return 3;
}

// CHECK:      last
// CHECK-NEXT: reenter
// CHECK-NOT:  last
// CHECK-NOT:  reenter
