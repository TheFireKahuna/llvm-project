// Test virtual inheritance vtables.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

// Diamond inheritance with virtual base.
class Base {
public:
  int base_value;

  Base(int v) : base_value(v) {
    printf("Base(%d) constructed\n", v);
  }

  virtual void identify() {
    printf("Base::identify (base_value=%d)\n", base_value);
  }

  virtual ~Base() {
    printf("Base destructed\n");
  }
};

class Left : virtual public Base {
public:
  int left_value;

  Left(int bv, int lv) : Base(bv), left_value(lv) {
    printf("Left(%d) constructed\n", lv);
  }

  void identify() override {
    printf("Left::identify (base_value=%d, left_value=%d)\n", base_value,
           left_value);
  }

  ~Left() override {
    printf("Left destructed\n");
  }
};

class Right : virtual public Base {
public:
  int right_value;

  Right(int bv, int rv) : Base(bv), right_value(rv) {
    printf("Right(%d) constructed\n", rv);
  }

  void identify() override {
    printf("Right::identify (base_value=%d, right_value=%d)\n", base_value,
           right_value);
  }

  ~Right() override {
    printf("Right destructed\n");
  }
};

class Diamond : public Left, public Right {
public:
  int diamond_value;

  Diamond(int bv, int lv, int rv, int dv)
      : Base(bv), Left(bv, lv), Right(bv, rv), diamond_value(dv) {
    printf("Diamond(%d) constructed\n", dv);
  }

  void identify() override {
    printf("Diamond::identify (base=%d, left=%d, right=%d, diamond=%d)\n",
           base_value, left_value, right_value, diamond_value);
  }

  ~Diamond() override {
    printf("Diamond destructed\n");
  }
};

int main() {
  // CHECK: Virtual inheritance test
  printf("Virtual inheritance test\n");

  {
    // CHECK: Base(100) constructed
    // CHECK: Left(10) constructed
    // CHECK: Right(20) constructed
    // CHECK: Diamond(1) constructed
    Diamond d(100, 10, 20, 1);

    // CHECK: Diamond::identify (base=100, left=10, right=20, diamond=1)
    d.identify();

    // Access through different base pointers.
    Base *bp = &d;
    Left *lp = &d;
    Right *rp = &d;

    // All should see the same base_value (only one Base subobject).
    // CHECK: via Base*: Diamond::identify
    printf("via Base*: ");
    bp->identify();

    // CHECK: via Left*: Diamond::identify
    printf("via Left*: ");
    lp->identify();

    // CHECK: via Right*: Diamond::identify
    printf("via Right*: ");
    rp->identify();

    // Verify single Base subobject.
    // CHECK: base_value matches across pointers = 1
    printf("base_value matches across pointers = %d\n",
           bp->base_value == lp->base_value &&
               lp->base_value == rp->base_value);
  }

  // CHECK: Diamond destructed
  // CHECK: Right destructed
  // CHECK: Left destructed
  // CHECK: Base destructed

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
