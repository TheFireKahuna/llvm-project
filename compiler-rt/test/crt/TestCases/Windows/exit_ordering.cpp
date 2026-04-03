// Test C++ destructors run before atexit handlers (Itanium ABI ordering).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

void atexit_handler_1() {
  printf("atexit_handler_1\n");
}

void atexit_handler_2() {
  printf("atexit_handler_2\n");
}

struct CppDestructor {
  int id;
  CppDestructor(int i) : id(i) {
    printf("CppDestructor(%d) constructed\n", id);
  }
  ~CppDestructor() {
    printf("CppDestructor(%d) destructed\n", id);
  }
};

CppDestructor global1(1);
CppDestructor global2(2);

int main() {
  atexit(atexit_handler_1);
  atexit(atexit_handler_2);

  printf("main() running\n");
  return 0;
}

// CHECK: CppDestructor(1) constructed
// CHECK: CppDestructor(2) constructed
// CHECK: main() running
// CHECK: CppDestructor(2) destructed
// CHECK: CppDestructor(1) destructed
// CHECK: atexit_handler_2
// CHECK: atexit_handler_1
