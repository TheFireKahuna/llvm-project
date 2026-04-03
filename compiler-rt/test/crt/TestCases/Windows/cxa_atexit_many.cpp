// Test __cxa_atexit with many registrations (exceeds DtorBlock capacity).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void *__dso_handle;

// DtorBlock has kDtorBlockSize = 32 entries. Register many more to test
// block overflow and heap allocation.
constexpr int kNumDtors = 100;

static int g_destructed_count = 0;
static int g_expected_order[kNumDtors];
static int g_actual_order[kNumDtors];

void counted_dtor(void *arg) {
  int idx = *static_cast<int *>(arg);
  g_actual_order[g_destructed_count] = idx;
  g_destructed_count++;
}

int main() {
  // CHECK: __cxa_atexit many registrations test
  printf("__cxa_atexit many registrations test\n");

  static int indices[kNumDtors];
  for (int i = 0; i < kNumDtors; ++i) {
    indices[i] = i;
    // Expected LIFO: last registered runs first.
    g_expected_order[i] = kNumDtors - 1 - i;
  }

  int failures = 0;
  for (int i = 0; i < kNumDtors; ++i) {
    if (__cxa_atexit(counted_dtor, &indices[i], __dso_handle) != 0)
      failures++;
  }

  // CHECK: all registrations succeeded = 1
  printf("all registrations succeeded = %d\n", failures == 0);

  // CHECK: registered count = 100
  printf("registered count = %d\n", kNumDtors);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Destructors run after main returns. Verify in static destructor.
struct Verifier {
  ~Verifier() {
    // CHECK: destructed count = 100
    printf("destructed count = %d\n", g_destructed_count);

    int order_correct = 1;
    for (int i = 0; i < kNumDtors; ++i) {
      if (g_actual_order[i] != g_expected_order[i]) {
        order_correct = 0;
        printf("order mismatch at %d: expected %d got %d\n", i,
               g_expected_order[i], g_actual_order[i]);
      }
    }
    // CHECK: LIFO order correct = 1
    printf("LIFO order correct = %d\n", order_correct);
  }
};

// Construct early to destruct after all __cxa_atexit handlers.
static Verifier g_verifier __attribute__((init_priority(101)));
