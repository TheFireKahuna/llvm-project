// Test thread-safe initialization (function-local statics).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

typedef void *HANDLE;
typedef unsigned long DWORD;
#define __stdcall __attribute__((ms_abi))

extern "C" {
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, size_t,
                                                     DWORD(__stdcall *)(void *),
                                                     void *, DWORD, DWORD *);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) void __stdcall Sleep(DWORD);
}

constexpr DWORD INFINITE_WAIT = 0xFFFFFFFF;

static int g_init_count = 0;

struct ExpensiveObject {
  int value;

  ExpensiveObject() : value(42) {
    g_init_count++;
    printf("ExpensiveObject constructed (count=%d)\n", g_init_count);
    // Simulate expensive initialization.
    Sleep(10);
  }

  ~ExpensiveObject() {
    printf("ExpensiveObject destructed\n");
  }
};

ExpensiveObject &get_singleton() {
  // Function-local static with thread-safe initialization (C++11 magic statics).
  static ExpensiveObject instance;
  return instance;
}

DWORD __stdcall thread_func(void *arg) {
  int thread_id = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  printf("[Thread %d] Requesting singleton\n", thread_id);

  ExpensiveObject &obj = get_singleton();

  printf("[Thread %d] Got singleton with value=%d\n", thread_id, obj.value);
  return 0;
}

int main() {
  // CHECK: Init-once thread safety test
  printf("Init-once thread safety test\n");

  constexpr int kNumThreads = 4;
  HANDLE threads[kNumThreads];

  // Launch multiple threads that all try to get the singleton.
  for (int i = 0; i < kNumThreads; ++i) {
    threads[i] = CreateThread(
        nullptr, 0, thread_func,
        reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)), 0, nullptr);
  }

  // Wait for all threads.
  for (int i = 0; i < kNumThreads; ++i) {
    WaitForSingleObject(threads[i], INFINITE_WAIT);
    CloseHandle(threads[i]);
  }

  // Despite multiple threads, singleton should only be constructed once.
  // CHECK: init count = 1
  printf("init count = %d\n", g_init_count);

  // All threads should have gotten value 42.
  // CHECK-COUNT-4: Got singleton with value=42

  // CHECK: PASS
  if (g_init_count == 1) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL - constructed %d times\n", g_init_count);
    return 1;
  }
}

// CHECK: ExpensiveObject destructed
