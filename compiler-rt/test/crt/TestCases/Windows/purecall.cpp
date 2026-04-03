// Test pure virtual call handling via _purecall -> __cxa_pure_virtual.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt
// XFAIL: *

#include <stdio.h>

struct Base {
  Base() {
    // Calling virtual from ctor invokes _purecall.
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
  printf("About to trigger pure virtual call\n");
  Derived d;
  printf("Should not reach here\n");
  return 0;
}

// CHECK: About to trigger pure virtual call
// CHECK-NOT: Should not reach here
