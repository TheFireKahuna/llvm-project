// Test basic vtable functionality with virtual functions.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

class Base {
public:
  virtual void method1() {
    printf("Base::method1\n");
  }
  virtual void method2() {
    printf("Base::method2\n");
  }
  virtual int compute(int x) {
    return x;
  }
  virtual ~Base() {
    printf("Base::~Base\n");
  }
};

class Derived : public Base {
public:
  void method1() override {
    printf("Derived::method1\n");
  }
  int compute(int x) override {
    return x * 2;
  }
  ~Derived() override {
    printf("Derived::~Derived\n");
  }
};

class MoreDerived : public Derived {
public:
  void method1() override {
    printf("MoreDerived::method1\n");
  }
  void method2() override {
    printf("MoreDerived::method2\n");
  }
  int compute(int x) override {
    return x * 3;
  }
  ~MoreDerived() override {
    printf("MoreDerived::~MoreDerived\n");
  }
};

void test_vtable(Base *obj, const char *name) {
  printf("Testing %s:\n", name);
  obj->method1();
  obj->method2();
  printf("compute(5) = %d\n", obj->compute(5));
}

int main() {
  // CHECK: Vtable basic test
  printf("Vtable basic test\n");

  {
    Base base;
    // CHECK: Testing Base:
    // CHECK: Base::method1
    // CHECK: Base::method2
    // CHECK: compute(5) = 5
    test_vtable(&base, "Base");
  }
  // CHECK: Base::~Base

  {
    Derived derived;
    // CHECK: Testing Derived:
    // CHECK: Derived::method1
    // CHECK: Base::method2
    // CHECK: compute(5) = 10
    test_vtable(&derived, "Derived");
  }
  // CHECK: Derived::~Derived
  // CHECK: Base::~Base

  {
    MoreDerived more;
    // CHECK: Testing MoreDerived:
    // CHECK: MoreDerived::method1
    // CHECK: MoreDerived::method2
    // CHECK: compute(5) = 15
    test_vtable(&more, "MoreDerived");
  }
  // CHECK: MoreDerived::~MoreDerived
  // CHECK: Derived::~Derived
  // CHECK: Base::~Base

  // Polymorphic dispatch through base pointer.
  Base *ptr = new MoreDerived();
  // CHECK: Testing dynamic MoreDerived:
  // CHECK: MoreDerived::method1
  // CHECK: MoreDerived::method2
  // CHECK: compute(5) = 15
  test_vtable(ptr, "dynamic MoreDerived");
  delete ptr;
  // CHECK: MoreDerived::~MoreDerived
  // CHECK: Derived::~Derived
  // CHECK: Base::~Base

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
