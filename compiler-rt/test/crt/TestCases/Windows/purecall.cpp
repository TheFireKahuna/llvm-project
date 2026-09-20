// Test pure virtual call handling via _purecall -> __cxa_pure_virtual.
//
// RUN: %clangxx_crt_main -std=c++17 %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
// RUN: %clangxx_crt_main -std=c++17 -DTEST_BRIDGE %s -o %t.bridge.exe
// RUN: not %run %t.bridge.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int _purecall();

struct Base {
  Base() {
    // The vtable's pure entry reports through __cxa_pure_virtual when no
    // handler is installed.
    call_pure();
  }

  virtual void pure_func() = 0;
  void call_pure() { pure_func(); }
  virtual ~Base() {}
};

struct Derived : Base {
  void pure_func() override {
    printf("Derived::pure_func called\n");
  }
};

int main() {
  // abort does not flush stdout redirected by lit.
  fprintf(stderr, "About to trigger pure virtual call\n");
#ifdef TEST_BRIDGE
  _purecall();
#else
  Derived d;
#endif
  fprintf(stderr, "Should not reach here\n");
  return 0;
}

// CHECK: About to trigger pure virtual call
// CHECK: Pure virtual function called!
// CHECK-NOT: Should not reach here
