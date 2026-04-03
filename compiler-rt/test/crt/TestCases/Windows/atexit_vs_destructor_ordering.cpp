// Test full exit ordering: thread_local -> static -> atexit.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

static int g_order = 0;

void atexit_handler_first() {
  printf("atexit_handler_first at order %d\n", g_order++);
}

void atexit_handler_second() {
  printf("atexit_handler_second at order %d\n", g_order++);
}

struct StaticDestructor {
  int id;
  StaticDestructor(int i) : id(i) {}
  ~StaticDestructor() {
    printf("StaticDestructor(%d) at order %d\n", id, g_order++);
  }
};

struct ThreadLocalDestructor {
  int id;
  ThreadLocalDestructor(int i) : id(i) {}
  ~ThreadLocalDestructor() {
    printf("ThreadLocalDestructor(%d) at order %d\n", id, g_order++);
  }
};

static StaticDestructor static1(1);
static StaticDestructor static2(2);

thread_local ThreadLocalDestructor tls1(10);
thread_local ThreadLocalDestructor tls2(20);

int main() {
  // CHECK: Full exit ordering test
  printf("Full exit ordering test\n");

  // Register atexit handlers early.
  atexit(atexit_handler_first);
  atexit(atexit_handler_second);

  // Access thread-locals.
  printf("tls1.id = %d, tls2.id = %d\n", tls1.id, tls2.id);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Itanium ABI exit ordering on Windows Itanium:
// 1. Thread-local destructors (LIFO)
// 2. Static destructors via __cxa_finalize (LIFO) - atexit() redirected here
// 3. Pre-terminators (.CRT$XPA-XPZ)
// 4. Terminators (.CRT$XTA-XTZ)
//
// Since atexit is redirected to __cxa_atexit, atexit handlers interleave with
// static destructors in global LIFO order:
// - static1 constructed, static2 constructed
// - atexit_first registered, atexit_second registered
// - tls1 constructed, tls2 constructed
//
// Exit order (all LIFO after thread-locals):
// tls2, tls1, atexit_second, atexit_first, static2, static1

// CHECK: ThreadLocalDestructor(20) at order 0
// CHECK: ThreadLocalDestructor(10) at order 1
// CHECK: atexit_handler_second at order 2
// CHECK: atexit_handler_first at order 3
// CHECK: StaticDestructor(2) at order 4
// CHECK: StaticDestructor(1) at order 5
