// Test _set_purecall_handler for custom pure virtual call handling.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

// _purecall_handler type and setter.
typedef void (*_purecall_handler)(void);
extern "C" _purecall_handler _get_purecall_handler(void);
extern "C" _purecall_handler _set_purecall_handler(_purecall_handler);

static int g_handler_called = 0;

void custom_purecall_handler() {
  g_handler_called++;
  printf("Custom purecall handler called (count=%d)\n", g_handler_called);
  // Handler should abort; if not, runtime calls abort anyway.
  abort();
}

struct Base {
  Base() {
    // Calling virtual during construction triggers purecall.
    call_virtual();
  }

  virtual void pure_method() = 0;

  void call_virtual() {
    // This indirection defeats devirtualization.
    pure_method();
  }

  virtual ~Base() = default;
};

struct Derived : Base {
  void pure_method() override {
    printf("Derived::pure_method - should not reach\n");
  }
};

int main() {
  // CHECK: Purecall handler test
  printf("Purecall handler test\n");

  // Get initial handler (should be nullptr or default).
  _purecall_handler old = _get_purecall_handler();
  // CHECK: initial handler: {{.*}}
  printf("initial handler: %p\n", (void *)old);

  // Set custom handler.
  _purecall_handler prev = _set_purecall_handler(custom_purecall_handler);
  // CHECK: previous handler: {{.*}}
  printf("previous handler: %p\n", (void *)prev);

  // Verify handler was set.
  _purecall_handler current = _get_purecall_handler();
  // CHECK: handler set correctly = 1
  printf("handler set correctly = %d\n",
         current == custom_purecall_handler);

  // CHECK: About to trigger purecall
  printf("About to trigger purecall\n");

  // This will trigger the purecall handler.
  Derived d;

  // Should not reach here.
  printf("Should not reach here\n");
  return 0;
}

// CHECK: Custom purecall handler called
// CHECK-NOT: Should not reach here
