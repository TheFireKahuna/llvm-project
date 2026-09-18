// Test exit codes are preserved correctly.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %python -c "import subprocess, sys; codes = [0, 1, 42, 255]; actual = [subprocess.call([sys.argv[1], str(c)]) for c in codes]; assert actual == codes, actual" %t.exe
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
