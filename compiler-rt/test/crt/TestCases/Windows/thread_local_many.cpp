// Test thread_local with many registrations (exceeds inline block capacity).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

// DtorBlock in cxa_thread_atexit.cpp has kEntriesPerBlock = 30.
// Create many more thread_local objects to test overflow.
constexpr int kNumObjects = 100;

static int g_construct_count = 0;
static int g_destruct_count = 0;
static int g_destruct_order[kNumObjects];

struct TlsObject {
  int id;
  TlsObject(int i) : id(i) {
    g_construct_count++;
  }
  ~TlsObject() {
    g_destruct_order[g_destruct_count++] = id;
  }
};

// Use a template to generate many distinct thread_local variables.
template <int N>
struct TlsHolder {
  static TlsObject &get() {
    thread_local TlsObject obj(N);
    return obj;
  }
};

// Access all the thread_locals.
template <int... Ns>
void access_all(std::integer_sequence<int, Ns...>) {
  // Fold expression to access each.
  ((void)TlsHolder<Ns>::get(), ...);
}

int main() {
  // CHECK: Thread-local many test
  printf("Thread-local many test\n");

  // Access all thread_local objects.
  access_all(std::make_integer_sequence<int, kNumObjects>{});

  // CHECK: constructed = 100
  printf("constructed = %d\n", g_construct_count);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Verify after main returns.
struct Verifier {
  ~Verifier() {
    // CHECK: destructed = 100
    printf("destructed = %d\n", g_destruct_count);

    // Verify LIFO order: last accessed should destruct first.
    // Objects are accessed 0, 1, 2, ..., so destruction is 99, 98, ...
    int lifo_correct = 1;
    for (int i = 0; i < kNumObjects; ++i) {
      int expected = kNumObjects - 1 - i;
      if (g_destruct_order[i] != expected) {
        printf("Order mismatch at %d: expected %d, got %d\n", i, expected,
               g_destruct_order[i]);
        lifo_correct = 0;
      }
    }
    // CHECK: LIFO order correct = 1
    printf("LIFO order correct = %d\n", lifo_correct);
  }
};

// High init_priority so it constructs early and destructs late.
static Verifier g_verifier __attribute__((init_priority(101)));
