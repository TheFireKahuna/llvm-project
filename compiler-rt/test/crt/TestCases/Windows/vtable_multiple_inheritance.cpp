// Test vtables with multiple inheritance.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

class InterfaceA {
public:
  virtual void doA() = 0;
  virtual ~InterfaceA() = default;
};

class InterfaceB {
public:
  virtual void doB() = 0;
  virtual int getValue() = 0;
  virtual ~InterfaceB() = default;
};

class InterfaceC {
public:
  virtual void doC() = 0;
  virtual ~InterfaceC() = default;
};

class Implementation : public InterfaceA, public InterfaceB, public InterfaceC {
  int value_;
public:
  explicit Implementation(int v) : value_(v) {
    printf("Implementation(%d) constructed\n", v);
  }

  void doA() override {
    printf("Implementation::doA (value=%d)\n", value_);
  }

  void doB() override {
    printf("Implementation::doB (value=%d)\n", value_);
  }

  int getValue() override {
    return value_;
  }

  void doC() override {
    printf("Implementation::doC (value=%d)\n", value_);
  }

  ~Implementation() override {
    printf("Implementation(%d) destructed\n", value_);
  }
};

void useA(InterfaceA *a) {
  printf("useA:\n");
  a->doA();
}

void useB(InterfaceB *b) {
  printf("useB:\n");
  b->doB();
  printf("  getValue() = %d\n", b->getValue());
}

void useC(InterfaceC *c) {
  printf("useC:\n");
  c->doC();
}

int main() {
  // CHECK: Multiple inheritance vtable test
  printf("Multiple inheritance vtable test\n");

  // CHECK: Implementation(42) constructed
  Implementation impl(42);

  // Cast to each base class and verify virtual dispatch works.
  // CHECK: useA:
  // CHECK: Implementation::doA (value=42)
  useA(&impl);

  // CHECK: useB:
  // CHECK: Implementation::doB (value=42)
  // CHECK: getValue() = 42
  useB(&impl);

  // CHECK: useC:
  // CHECK: Implementation::doC (value=42)
  useC(&impl);

  // Verify through dynamic allocation and base pointers.
  InterfaceA *pA = new Implementation(100);
  InterfaceB *pB = dynamic_cast<InterfaceB *>(pA);
  InterfaceC *pC = dynamic_cast<InterfaceC *>(pA);

  // CHECK: Implementation(100) constructed

  if (pB) {
    // CHECK: dynamic_cast to B succeeded
    printf("dynamic_cast to B succeeded\n");
    // CHECK: getValue via cast = 100
    printf("getValue via cast = %d\n", pB->getValue());
  }

  if (pC) {
    // CHECK: dynamic_cast to C succeeded
    printf("dynamic_cast to C succeeded\n");
    pC->doC();
    // CHECK: Implementation::doC (value=100)
  }

  delete pA;
  // CHECK: Implementation(100) destructed

  // CHECK: Implementation(42) destructed
  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
