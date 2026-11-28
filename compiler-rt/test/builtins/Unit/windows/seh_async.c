// RUN: %clang -fasync-exceptions -O0 %s -o %t-O0.exe
// RUN: %run %t-O0.exe | FileCheck %s
// RUN: %clang -fasync-exceptions -O2 %s -o %t-O2.exe
// RUN: %run %t-O2.exe | FileCheck %s

// Under -fasync-exceptions a __try block covers each of its instructions, not
// only its calls: a fault in the block itself reaches the block's filter and
// handler, in a function that reads a thread-local variable and in a variadic
// one. What the function holds from before the block, such as the address of
// a thread-local variable, is still held after the handler.

// CHECK: 3 3 7 7

#include <stdarg.h>
#include <stdio.h>

static _Thread_local int T = 3;
static int *volatile Null;
static volatile int Filtered, Handled;

__attribute__((noinline)) static int filter(void) { return ++Filtered > 0; }

__attribute__((noinline)) static void plain(void) {
  T++;
  __try {
    *Null = 1;
  } __except (filter()) {
    Handled++;
  }
}

__attribute__((noinline)) static void variadic(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int v = va_arg(ap, int);
  __try {
    T += v;
    *Null = 1;
  } __except (filter()) {
    Handled++;
  }
  va_end(ap);
}

__attribute__((noinline)) static int holds(void) {
  T++;
  __try {
    *Null = 1;
  } __except (filter()) {
    Handled++;
  }
  return T;
}

int main(void) {
  plain();
  variadic(1, 2);
  int H = holds();
  printf("%d %d %d %d\n", Filtered, Handled, T, H);
  return 0;
}
