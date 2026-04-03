// Test basic thread_local destructor support via __cxa_thread_atexit_impl.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

struct TlsCounter {
  int id;
  TlsCounter(int i) : id(i) {
    printf("TlsCounter(%d) constructed\n", id);
  }
  ~TlsCounter() {
    printf("TlsCounter(%d) destructed\n", id);
  }
};

thread_local TlsCounter tls1(1);
thread_local TlsCounter tls2(2);
thread_local TlsCounter tls3(3);

int main() {
  // CHECK: Thread-local destructor test
  printf("Thread-local destructor test\n");

  // Access thread_local to trigger construction.
  // CHECK: TlsCounter(1) constructed
  // CHECK: TlsCounter(2) constructed
  // CHECK: TlsCounter(3) constructed

  // CHECK: tls1.id = 1
  printf("tls1.id = %d\n", tls1.id);

  // CHECK: tls2.id = 2
  printf("tls2.id = %d\n", tls2.id);

  // CHECK: tls3.id = 3
  printf("tls3.id = %d\n", tls3.id);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Thread-local destructors run in LIFO order on main thread exit.
// CHECK: TlsCounter(3) destructed
// CHECK: TlsCounter(2) destructed
// CHECK: TlsCounter(1) destructed
