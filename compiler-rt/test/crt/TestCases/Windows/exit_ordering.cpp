// Test C++ destructors and atexit handlers interleave in reverse registration
// order, as required by [basic.start.term] and Itanium ABI section 3.3.6.
//
// RUN: %clangxx_crt_main -std=c++17 %s -o %t.exe
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
  static CppDestructor local(3);
  atexit(atexit_handler_2);

  printf("main() running\n");
  return 0;
}

// CHECK: CppDestructor(1) constructed
// CHECK: CppDestructor(2) constructed
// CHECK: CppDestructor(3) constructed
// CHECK: main() running
// CHECK: atexit_handler_2
// CHECK: CppDestructor(3) destructed
// CHECK: atexit_handler_1
// CHECK: CppDestructor(2) destructed
// CHECK: CppDestructor(1) destructed
