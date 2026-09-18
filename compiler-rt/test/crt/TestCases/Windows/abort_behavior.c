// Test abort() behavior.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  // CHECK: About to abort
  printf("About to abort\n");
  fflush(stdout);

  abort();

  // Should not reach here.
  printf("Should not print\n");
  return 0;
}

// CHECK-NOT: Should not print
