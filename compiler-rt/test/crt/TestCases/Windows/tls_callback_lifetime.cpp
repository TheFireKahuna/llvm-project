// A TLS directory must point past the opening null sentinel, so that the
// loader actually invokes the module's callbacks on thread attach/detach.
// RUN: %clangxx_crt_main -O2 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// REQUIRES: windows, crt

#include <atomic>
#include <stdio.h>
#include <thread>
#include <windows.h>

static std::atomic<int> attaches(0), detaches(0);

static void NTAPI callback(void *, DWORD reason, void *) {
  if (reason == DLL_THREAD_ATTACH)
    attaches.fetch_add(1, std::memory_order_relaxed);
  if (reason == DLL_THREAD_DETACH)
    detaches.fetch_add(1, std::memory_order_relaxed);
}

#pragma section(".CRT$XLY", long, read)
extern "C"
    __declspec(allocate(".CRT$XLY")) PIMAGE_TLS_CALLBACK test_tls_callback =
        callback;
#if defined(__i386__)
#  pragma comment(linker, "/INCLUDE:_test_tls_callback")
#else
#  pragma comment(linker, "/INCLUDE:test_tls_callback")
#endif

extern "C" const IMAGE_TLS_DIRECTORY _tls_used;

int main() {
  auto callbacks =
      reinterpret_cast<PIMAGE_TLS_CALLBACK *>(_tls_used.AddressOfCallBacks);
  printf("first callback present = %d\n", callbacks && callbacks[0]);
  std::thread([] {}).join();
  printf("thread attaches = %d\n", attaches.load());
  printf("thread detaches = %d\n", detaches.load());
}

// CHECK: first callback present = 1
// CHECK-NEXT: thread attaches = 1
// CHECK-NEXT: thread detaches = 1
