// Test environment variable handling and _environ global.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **_environ;

int main(int argc, char **argv, char **envp) {
  printf("envp is not NULL = %d\n", envp != NULL);
  // CHECK: envp is not NULL = 1

  printf("_environ matches envp = %d\n", _environ == envp);
  // CHECK: _environ matches envp = 1

  int env_count = 0;
  for (char **p = envp; *p != NULL; p++) {
    env_count++;
  }
  printf("environment variable count > 0 = %d\n", env_count > 0);
  // CHECK: environment variable count > 0 = 1

  int found_path = 0;
  int found_systemroot = 0;

  for (char **p = envp; *p != NULL; p++) {
    if (_strnicmp(*p, "PATH=", 5) == 0) {
      found_path = 1;
    }
    if (_strnicmp(*p, "SYSTEMROOT=", 11) == 0) {
      found_systemroot = 1;
    }
  }

  printf("found PATH = %d\n", found_path);
  printf("found SYSTEMROOT = %d\n", found_systemroot);
  // CHECK: found PATH = 1
  // CHECK: found SYSTEMROOT = 1

  char *path = getenv("PATH");
  printf("getenv(PATH) works = %d\n", path != NULL);
  // CHECK: getenv(PATH) works = 1

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
