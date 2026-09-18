// Test invalid parameter handler terminates on UCRT validation errors.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: not %run %t.exe 2>&1
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  printf("Testing invalid parameter handler\n");
  fflush(stdout);

  // Null format triggers invalid parameter handler.
  printf(NULL);

  printf("ERROR: Should not reach this point\n");
  return 1;
}
