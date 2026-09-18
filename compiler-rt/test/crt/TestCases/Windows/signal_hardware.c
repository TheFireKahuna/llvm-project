// Hardware exceptions reaching the executable's entry frame are delivered as
// UCRT signals, as with vcruntime's startup. Without a handler the exception
// stays unhandled and ends the process.
// RUN: %clang_crt_main -O0 %s -o %t.exe
// RUN: %run %t.exe segv 2>&1 | FileCheck %s --check-prefix=SEGV
// RUN: %run %t.exe ill 2>&1 | FileCheck %s --check-prefix=ILL
// RUN: not --crash %run %t.exe default
// REQUIRES: windows, crt

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static void handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  // Returning would resume the faulting instruction.
  _Exit(0);
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (argv[1][0] != 'd') {
    signal(SIGSEGV, handler);
    signal(SIGILL, handler);
  }
  fprintf(stderr, "faulting\n");
  if (argv[1][0] == 'i')
    __builtin_trap();
  *(volatile int *)0 = 1;
  return 1;
}

// SEGV: faulting
// SEGV-NEXT: signal 11
// ILL: faulting
// ILL-NEXT: signal 4
