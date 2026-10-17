// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// An exception that no frame of the program handles reaches the Universal
// CRT's signal handlers through the entry point's filter.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static void handler(int Signal) {
  printf("signal %d\n", Signal);
  fflush(stdout);
  _exit(0);
}

int main(void) {
  signal(SIGSEGV, handler);
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
  return 1;
}

// CHECK: signal 11
