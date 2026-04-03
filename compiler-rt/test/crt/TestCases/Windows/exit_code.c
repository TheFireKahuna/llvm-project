// Test exit codes are preserved correctly.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe 0 ; test $? -eq 0
// RUN: %run %t.exe 1 ; test $? -eq 1
// RUN: %run %t.exe 42 ; test $? -eq 42
// RUN: %run %t.exe 255 ; test $? -eq 255
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: %s <exit_code>\n", argv[0]);
    return 1;
  }

  int code = atoi(argv[1]);
  printf("Exiting with code %d\n", code);
  return code;
}
