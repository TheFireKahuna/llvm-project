// Integration test for compiler-rt mode on Windows Itanium.
// Tests that CRT initialization, C++ runtime, and builtins work together.
//
// RUN: %clangxx_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <typeinfo>

// Global with static initialization.
static int g_init_order = 0;

struct StaticInit {
  int order;
  StaticInit() : order(++g_init_order) {
    printf("StaticInit constructed: order=%d\n", order);
  }
  ~StaticInit() { printf("StaticInit destroyed: order=%d\n", order); }
};

static StaticInit g_static1;
static StaticInit g_static2;

// Class for RTTI testing.
class Base {
public:
  virtual ~Base() = default;
  virtual const char *name() const { return "Base"; }
};

class Derived : public Base {
public:
  const char *name() const override { return "Derived"; }
};

// Atexit handler.
void atexit_handler() { printf("atexit handler called\n"); }

// Test exception handling.
void throw_and_catch() {
  try {
    throw 42;
  } catch (int e) {
    printf("Caught exception: %d\n", e);
  }
}

// Test exception with destructor unwinding.
struct Unwindable {
  int id;
  Unwindable(int i) : id(i) { printf("Unwindable %d constructed\n", id); }
  ~Unwindable() { printf("Unwindable %d destroyed\n", id); }
};

void throw_with_unwind() {
  Unwindable u1(1);
  Unwindable u2(2);
  throw "test";
}

int main() {
  // CHECK: StaticInit constructed: order=1
  // CHECK: StaticInit constructed: order=2

  // CHECK: Integration test start
  printf("Integration test start\n");

  // Test atexit registration.
  atexit(atexit_handler);

  // Test C++ memory allocation.
  int *p = new int(42);
  // CHECK: new/delete: 42
  printf("new/delete: %d\n", *p);
  delete p;

  // Test array new/delete.
  int *arr = new int[10];
  for (int i = 0; i < 10; ++i)
    arr[i] = i;
  // CHECK: array sum: 45
  int sum = 0;
  for (int i = 0; i < 10; ++i)
    sum += arr[i];
  printf("array sum: %d\n", sum);
  delete[] arr;

  // Test RTTI.
  Base *b = new Derived();
  // CHECK: typeid: {{.*}}Derived
  printf("typeid: %s\n", typeid(*b).name());
  // CHECK: dynamic_cast succeeded
  if (dynamic_cast<Derived *>(b))
    printf("dynamic_cast succeeded\n");
  delete b;

  // Test exception handling.
  // CHECK: Caught exception: 42
  throw_and_catch();

  // Test stack unwinding.
  // CHECK: Unwindable 1 constructed
  // CHECK: Unwindable 2 constructed
  // CHECK: Unwindable 2 destroyed
  // CHECK: Unwindable 1 destroyed
  // CHECK: Caught string exception
  try {
    throw_with_unwind();
  } catch (const char *msg) {
    printf("Caught string exception\n");
  }

  // Test 64-bit arithmetic (builtins).
  int64_t big = 1LL << 40;
  int64_t div = big / 1000;
  // CHECK: 64-bit arithmetic: 1099511627
  printf("64-bit arithmetic: %lld\n", (long long)div);

  // Test bit operations.
  // CHECK: popcount(0xFF00FF00) = 16
  printf("popcount(0xFF00FF00) = %d\n", __builtin_popcount(0xFF00FF00));

  // CHECK: Integration test end
  printf("Integration test end\n");

  // CHECK: atexit handler called
  // CHECK: StaticInit destroyed: order=2
  // CHECK: StaticInit destroyed: order=1
  return 0;
}
