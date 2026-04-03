// Test thread_local with dynamic initialization.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

struct TlsObject {
  int value;
  TlsObject(int v) : value(v) {
    printf("TlsObject(%d) constructed\n", v);
  }
  ~TlsObject() {
    printf("TlsObject(%d) destructed\n", value);
  }
};

thread_local TlsObject tls_obj(42);
thread_local int tls_int = 100;

int main() {
  // CHECK: TlsObject(42) constructed
  // CHECK: tls_obj.value = 42
  printf("tls_obj.value = %d\n", tls_obj.value);

  // CHECK: tls_int = 100
  printf("tls_int = %d\n", tls_int);

  tls_obj.value = 999;
  tls_int = 200;

  // CHECK: tls_obj.value modified = 999
  printf("tls_obj.value modified = %d\n", tls_obj.value);

  // CHECK: tls_int modified = 200
  printf("tls_int modified = %d\n", tls_int);

  // CHECK: main() done
  printf("main() done\n");

  return 0;
}

// CHECK: TlsObject(999) destructed
