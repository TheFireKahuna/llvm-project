// Exercise the public UCRT jump buffer through real PE frames, including CFG.
// C++ uses only trivial objects: no nontrivial destructor may be skipped.
// RUN: %clang_crt_main -std=c17 -O0 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// RUN: %clang_crt_main -std=c17 -O2 -flto=thin -mguard=cf -UNDEBUG %s -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe
// RUN: %clangxx_crt_main -x c++ -std=c++20 -O2 -mguard=cf -UNDEBUG %s -o %t.cxx.exe -Wl,/guard:cf
// RUN: %run %t.cxx.exe
// RUN: %clangxx_crt_main -x c++ -std=c++03 -D_LIBCPP_USE_FROZEN_CXX03_HEADERS -O2 -UNDEBUG %s -o %t.cxx03.exe
// RUN: %run %t.cxx03.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <setjmp.h>

static jmp_buf saved;

__attribute__((noinline)) static void jump(int value, unsigned depth,
                                           volatile int *changed) {
  volatile unsigned char frame[256];
  frame[0] = (unsigned char)depth;
  if (depth)
    jump(value, depth - 1, changed);
  *changed = 73 + frame[0];
  longjmp(saved, value);
}

static void check(int value) {
  volatile int changed = 0;
  switch (setjmp(saved)) {
  case 0:
    jump(value, 4, &changed);
    assert(0);
    break;
  case 1:
    assert(value == 0);
    break;
  case 42:
    assert(value == 42);
    break;
  default:
    assert(0);
  }
  assert(changed == 73);
}

int main(void) {
  for (int i = 0; i != 64; ++i) {
    check(0);
    check(42);
  }
}
