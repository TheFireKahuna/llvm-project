// Test C++ static destructor LIFO ordering.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

struct TestObject {
  int id;
  TestObject(int i) : id(i) {
    printf("TestObject(%d) constructed\n", id);
  }
  ~TestObject() {
    printf("TestObject(%d) destructed\n", id);
  }
};

TestObject obj1(1);
TestObject obj2(2);
TestObject obj3(3);

int main() {
  printf("main() running\n");
  return 0;
}

// CHECK: TestObject(1) constructed
// CHECK: TestObject(2) constructed
// CHECK: TestObject(3) constructed
// CHECK: main() running
// CHECK: TestObject(3) destructed
// CHECK: TestObject(2) destructed
// CHECK: TestObject(1) destructed
