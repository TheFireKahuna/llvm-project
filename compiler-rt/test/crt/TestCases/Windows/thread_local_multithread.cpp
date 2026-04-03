// Test thread_local destructors across multiple threads.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe -pthread
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

// Minimal thread API without windows.h.
typedef void *HANDLE;
typedef unsigned long DWORD;
#define __stdcall __attribute__((ms_abi))

extern "C" {
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, size_t,
                                                     DWORD(__stdcall *)(void *),
                                                     void *, DWORD, DWORD *);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) DWORD __stdcall GetCurrentThreadId(void);
}

constexpr DWORD INFINITE_WAIT = 0xFFFFFFFF;

struct TlsObject {
  int id;
  DWORD thread_id;

  TlsObject(int i) : id(i), thread_id(GetCurrentThreadId()) {
    printf("[Thread %lu] TlsObject(%d) constructed\n", thread_id, id);
  }

  ~TlsObject() {
    printf("[Thread %lu] TlsObject(%d) destructed\n", thread_id, id);
  }
};

thread_local TlsObject tls_obj(42);

// Per-thread counter to verify each thread gets its own storage.
thread_local int tls_counter = 0;

DWORD __stdcall thread_func(void *arg) {
  int thread_num = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  printf("[Thread %lu] Starting thread %d\n", GetCurrentThreadId(), thread_num);

  // Access thread_local variables - should trigger construction for this thread.
  printf("[Thread %lu] tls_obj.id = %d\n", GetCurrentThreadId(), tls_obj.id);

  // Each thread has its own counter.
  for (int i = 0; i < thread_num; ++i) {
    tls_counter++;
  }
  printf("[Thread %lu] tls_counter = %d\n", GetCurrentThreadId(), tls_counter);

  printf("[Thread %lu] Thread %d exiting\n", GetCurrentThreadId(), thread_num);
  return 0;
}

int main() {
  // CHECK: Thread-local multithread test
  printf("Thread-local multithread test\n");

  DWORD main_tid = GetCurrentThreadId();
  // CHECK: [Thread {{[0-9]+}}] Main thread
  printf("[Thread %lu] Main thread\n", main_tid);

  // Access thread_local on main thread.
  // CHECK: [Thread {{[0-9]+}}] TlsObject(42) constructed
  printf("[Thread %lu] tls_obj.id = %d\n", main_tid, tls_obj.id);

  constexpr int kNumThreads = 3;
  HANDLE threads[kNumThreads];

  for (int i = 0; i < kNumThreads; ++i) {
    threads[i] =
        CreateThread(nullptr, 0, thread_func,
                     reinterpret_cast<void *>(static_cast<intptr_t>(i + 1)),
                     0, nullptr);
    if (!threads[i]) {
      printf("Failed to create thread %d\n", i);
      return 1;
    }
  }

  // Wait for all threads to complete.
  for (int i = 0; i < kNumThreads; ++i) {
    WaitForSingleObject(threads[i], INFINITE_WAIT);
    CloseHandle(threads[i]);
  }

  // Main thread's counter should be unaffected.
  // CHECK: [Thread {{[0-9]+}}] main tls_counter = 0
  printf("[Thread %lu] main tls_counter = %d\n", main_tid, tls_counter);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Each worker thread should have constructed and destructed its own TlsObject.
// CHECK-DAG: TlsObject(42) constructed
// CHECK-DAG: TlsObject(42) destructed
// Main thread's TlsObject destructs at process exit.
// CHECK: TlsObject(42) destructed
