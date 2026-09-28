// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// An executable's _matherr receives the math library's errors.

#include <math.h>
#include <stdio.h>

int _matherr(struct _exception *Exception) {
  printf("%s %d\n", Exception->name, Exception->type);
  return 1;
}

int main(void) {
  volatile double Argument = -1.0;
  (void)log(Argument);
  return 0;
}

// CHECK: log 1
