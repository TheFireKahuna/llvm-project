// Test that destructor exceptions during cleanup call std::terminate.
//
// RUN: %clangxx_crt_main -std=c++17 -fexceptions %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// RUN: not %run %t.exe catch 2>&1 | FileCheck %s
// REQUIRES: windows, crt

#include <exception>
#include <stdio.h>
#include <stdlib.h>

void custom_terminate() {
  fprintf(stderr, "custom_terminate called\n");
  auto exception = std::current_exception();
  if (!exception)
    _Exit(0);
  try {
    std::rethrow_exception(exception);
  } catch (int value) {
    if (value != 42)
      _Exit(0);
  } catch (...) {
    _Exit(0);
  }
  fprintf(stderr, "active exception = 42\n");
  abort();
}

struct ThrowingDestructor {
  int id;
  bool should_throw;

  ThrowingDestructor(int i, bool t) : id(i), should_throw(t) {
    fprintf(stderr, "ThrowingDestructor(%d) constructed\n", id);
  }

  ~ThrowingDestructor() noexcept(false) {
    fprintf(stderr, "ThrowingDestructor(%d) destructor called\n", id);
    if (should_throw) {
      fprintf(stderr, "ThrowingDestructor(%d) about to throw\n", id);
      throw 42;  // Throwing during exit cleanup should call terminate.
    }
    fprintf(stderr, "ThrowingDestructor(%d) destructor complete\n", id);
  }
};

// Static object with throwing destructor.
static ThrowingDestructor thrower(1, true);

int main(int argc, char **) {
  // CHECK: ThrowingDestructor(1) constructed
  // CHECK: Destructor exception test
  fprintf(stderr, "Destructor exception test\n");

  // Install custom terminate handler.
  std::set_terminate(custom_terminate);

  // CHECK: main done
  fprintf(stderr, "main done\n");
  if (argc > 1) {
    try {
      exit(0);
    } catch (...) {
      _Exit(0); // Escaping into the caller must fail the `not` RUN line.
    }
  }
  return 0;
}

// An exception escaping a static destructor during termination must call
// std::terminate, including the installed terminate handler.
// CHECK: ThrowingDestructor(1) destructor called
// CHECK: ThrowingDestructor(1) about to throw
// CHECK: custom_terminate called
// CHECK-NEXT: active exception = 42
