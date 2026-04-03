// Test global new/delete operators work correctly.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <new>

// Allocation tracking.
static int g_alloc_count = 0;
static int g_dealloc_count = 0;
static size_t g_total_allocated = 0;

struct TrackedObject {
  int value;
  char padding[60];  // Make it larger than typical small object optimization.

  TrackedObject(int v) : value(v) {
    printf("TrackedObject(%d) constructed\n", v);
  }

  ~TrackedObject() {
    printf("TrackedObject(%d) destructed\n", value);
  }
};

int main() {
  // CHECK: Global new/delete test
  printf("Global new/delete test\n");

  // Test basic new/delete.
  // CHECK: TrackedObject(42) constructed
  TrackedObject *single = new TrackedObject(42);
  // CHECK: single->value = 42
  printf("single->value = %d\n", single->value);
  delete single;
  // CHECK: TrackedObject(42) destructed

  // Test array new/delete.
  // CHECK: TrackedObject(1) constructed
  // CHECK: TrackedObject(2) constructed
  // CHECK: TrackedObject(3) constructed
  TrackedObject *arr = new TrackedObject[3]{{1}, {2}, {3}};
  // CHECK: arr[0].value = 1
  printf("arr[0].value = %d\n", arr[0].value);
  // CHECK: arr[1].value = 2
  printf("arr[1].value = %d\n", arr[1].value);
  // CHECK: arr[2].value = 3
  printf("arr[2].value = %d\n", arr[2].value);
  delete[] arr;
  // CHECK: TrackedObject(3) destructed
  // CHECK: TrackedObject(2) destructed
  // CHECK: TrackedObject(1) destructed

  // Test nothrow new.
  int *nothrow_ptr = new (std::nothrow) int(123);
  // CHECK: nothrow allocation = 1
  printf("nothrow allocation = %d\n", nothrow_ptr != nullptr);
  // CHECK: nothrow value = 123
  printf("nothrow value = %d\n", nothrow_ptr ? *nothrow_ptr : -1);
  delete nothrow_ptr;

  // Test placement new.
  alignas(TrackedObject) char buffer[sizeof(TrackedObject)];
  // CHECK: TrackedObject(999) constructed
  TrackedObject *placed = new (buffer) TrackedObject(999);
  // CHECK: placed->value = 999
  printf("placed->value = %d\n", placed->value);
  placed->~TrackedObject();  // Explicit destructor call for placement.
  // CHECK: TrackedObject(999) destructed

  // Test large allocation.
  constexpr size_t kLargeSize = 1024 * 1024;  // 1 MB
  char *large = new char[kLargeSize];
  // CHECK: large allocation succeeded = 1
  printf("large allocation succeeded = %d\n", large != nullptr);
  // Write to verify it's usable.
  large[0] = 'A';
  large[kLargeSize - 1] = 'Z';
  // CHECK: large write verified = 1
  printf("large write verified = %d\n",
         large[0] == 'A' && large[kLargeSize - 1] == 'Z');
  delete[] large;

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
