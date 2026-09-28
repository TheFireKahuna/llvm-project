// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe quick_exit | FileCheck %s --check-prefix=QUICK
// RUN: %run %t.exe _Exit | FileCheck %s --check-prefix=EXIT --allow-empty

// quick_exit runs the at_quick_exit registrations in reverse order and no
// atexit one; _Exit runs neither.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void atexitFunction(void) { printf("atexit\n"); }
static void quick1(void) { printf("at_quick_exit 1\n"); }
static void quick2(void) {
  printf("at_quick_exit 2\n");
  fflush(stdout);
}

int main(int argc, char **argv) {
  atexit(atexitFunction);
  at_quick_exit(quick2);
  at_quick_exit(quick1);
  fflush(stdout);
  if (argc == 2 && !strcmp(argv[1], "_Exit"))
    _Exit(0);
  quick_exit(0);
}

// QUICK:      at_quick_exit 1
// QUICK-NEXT: at_quick_exit 2
// QUICK-NOT:  atexit

// EXIT-NOT: {{.}}
