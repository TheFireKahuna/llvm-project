// Stress test for atexit with high registration count.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

// Test with a large number of atexit registrations.
constexpr int kNumHandlers = 1000;

static int g_call_count = 0;
static int g_last_index = -1;
static int g_lifo_violations = 0;

// Store expected order (registration order).
static int g_registered_indices[kNumHandlers];

void handler() {
  // Each handler is uniquely identified by its index in g_registered_indices.
  // At call time, we need to verify LIFO order.
  int call_num = g_call_count++;

  // Expected: handlers called in reverse registration order.
  // Handler registered at index i should be called at position (kNumHandlers - 1 - i).
  // So the handler called at position call_num was registered at index
  // (kNumHandlers - 1 - call_num).

  // We can't easily identify which handler this is without storing state,
  // but we can verify the count at the end.
}

// Use a template to create unique handlers that track their index.
template <int Index>
void indexed_handler() {
  int call_num = g_call_count++;
  int expected_registration_index = kNumHandlers - 1 - call_num;

  if (Index != expected_registration_index) {
    g_lifo_violations++;
  }
}

// Generate registrations.
template <int... Indices>
void register_all(std::integer_sequence<int, Indices...>) {
  // Register handlers 0, 1, 2, ..., kNumHandlers-1.
  (atexit(indexed_handler<Indices>), ...);
}

int main() {
  // CHECK: atexit stress test
  printf("atexit stress test\n");

  // CHECK: registering 1000 handlers
  printf("registering %d handlers\n", kNumHandlers);

  // Register all handlers.
  register_all(std::make_integer_sequence<int, kNumHandlers>{});

  // CHECK: all registered
  printf("all registered\n");

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Verify results after all handlers run.
struct Verifier {
  ~Verifier() {
    // CHECK: handlers called = 1000
    printf("handlers called = %d\n", g_call_count);

    // CHECK: LIFO violations = 0
    printf("LIFO violations = %d\n", g_lifo_violations);

    // CHECK: stress test PASS
    if (g_call_count == kNumHandlers && g_lifo_violations == 0) {
      printf("stress test PASS\n");
    } else {
      printf("stress test FAIL\n");
    }
  }
};

// Early init_priority so it destructs after all atexit handlers.
static Verifier g_verifier __attribute__((init_priority(101)));
