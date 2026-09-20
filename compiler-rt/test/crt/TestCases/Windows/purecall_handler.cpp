// Test _set_purecall_handler for custom pure virtual call handling.
//
// RUN: %clangxx_crt_main -std=c++17 %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

// _purecall_handler type and setter.
typedef void (*_purecall_handler)(void);
extern "C" int _purecall(void);
extern "C" _purecall_handler _get_purecall_handler(void);
extern "C" _purecall_handler _set_purecall_handler(_purecall_handler);

struct Base {
  Base() { call_pure(); }
  virtual void pure_func() = 0;
  void call_pure() { pure_func(); }
  virtual ~Base() {}
};

struct Derived : Base {
  void pure_func() override {}
};

static int g_handler_called = 0;

void custom_purecall_handler() {
  g_handler_called++;
  fprintf(stderr, "Custom purecall handler called (count=%d)\n", g_handler_called);
  // Handler should abort; if not, runtime calls abort anyway.
  abort();
}

int main() {
  // CHECK: Purecall handler test
  fprintf(stderr, "Purecall handler test\n");

  // Get initial handler (should be nullptr or default).
  _purecall_handler old = _get_purecall_handler();
  // CHECK: initial handler: {{.*}}
  fprintf(stderr, "initial handler: %p\n", (void *)old);

  // Set custom handler.
  _purecall_handler prev = _set_purecall_handler(custom_purecall_handler);
  // CHECK: previous handler matches = 1
  fprintf(stderr, "previous handler matches = %d\n", prev == old);

  // Verify handler was set.
  _purecall_handler current = _get_purecall_handler();
  // CHECK: handler set correctly = 1
  fprintf(stderr, "handler set correctly = %d\n",
         current == custom_purecall_handler);

  // CHECK: About to trigger purecall
  fprintf(stderr, "About to trigger purecall\n");

  // A vtable's pure entry names the entry point that owns this handler, so
  // a real pure virtual call reaches it as it does with code built by MSVC.
  Derived d;
  (void)d;

  // Should not reach here.
  fprintf(stderr, "Should not reach here\n");
  return 0;
}

// CHECK: Custom purecall handler called
// CHECK-NOT: Should not reach here
