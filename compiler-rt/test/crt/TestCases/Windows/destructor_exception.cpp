// Test that destructor exceptions during cleanup call std::terminate.
//
// RUN: %clang_crt_main -std=c++17 -fexceptions %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <exception>
#include <stdio.h>
#include <stdlib.h>

static bool g_terminate_called = false;

void custom_terminate() {
  printf("custom_terminate called\n");
  g_terminate_called = true;
  abort();
}

struct ThrowingDestructor {
  int id;
  bool should_throw;

  ThrowingDestructor(int i, bool t) : id(i), should_throw(t) {
    printf("ThrowingDestructor(%d) constructed\n", id);
  }

  ~ThrowingDestructor() noexcept(false) {
    printf("ThrowingDestructor(%d) destructor called\n", id);
    if (should_throw) {
      printf("ThrowingDestructor(%d) about to throw\n", id);
      throw 42;  // Throwing during exit cleanup should call terminate.
    }
    printf("ThrowingDestructor(%d) destructor complete\n", id);
  }
};

// Static object with throwing destructor.
static ThrowingDestructor thrower(1, true);

int main() {
  // CHECK: Destructor exception test
  printf("Destructor exception test\n");

  // Install custom terminate handler.
  std::set_terminate(custom_terminate);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Per Itanium ABI 3.3.5: throwing from a destructor during __cxa_finalize
// causes terminate() to be called.

// CHECK: ThrowingDestructor(1) constructed
// CHECK: main done
// CHECK: ThrowingDestructor(1) destructor called
// CHECK: ThrowingDestructor(1) about to throw
// CHECK: custom_terminate called
