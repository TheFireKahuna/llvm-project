// exit destroys the calling thread's TLS; quick_exit destroys neither thread's.
// RUN: %clangxx_crt_main -std=c++17 -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe normal std
// RUN: %run %t.exe normal native
// RUN: %run %t.exe normal ucrt
// RUN: %run %t.exe quick std
// RUN: %run %t.exe quick native
// RUN: %run %t.exe quick ucrt
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <process.h>
#include <stdlib.h>
#include <thread>
#include <windows.h>

static LONG mainDestroyed, workerDestroyed, automaticDestroyed;
static DWORD terminatingThread;
struct Object {
  LONG *counter;
  ~Object() { InterlockedIncrement(counter); }
};
static void normal() {
  assert(GetCurrentThreadId() == terminatingThread);
  assert(workerDestroyed == 1 && mainDestroyed == 0 && automaticDestroyed == 0);
}
static void quick() {
  assert(GetCurrentThreadId() == terminatingThread);
  assert(workerDestroyed == 0 && mainDestroyed == 0 && automaticDestroyed == 0);
}
static void leave(void *mode) {
  thread_local Object local{&workerDestroyed};
  (void)local;
  Object automatic{&automaticDestroyed};
  terminatingThread = GetCurrentThreadId();
  if (*static_cast<char *>(mode) == 'q')
    quick_exit(0);
  exit(0);
}
static DWORD WINAPI native(void *mode) {
  leave(mode);
  return 1;
}
static unsigned __stdcall ucrt(void *mode) {
  leave(mode);
  return 1;
}
int main(int argc, char **argv) {
  assert(argc == 3);
  thread_local Object local{&mainDestroyed};
  (void)local;
  assert(atexit(normal) == 0 && at_quick_exit(quick) == 0);
  if (argv[2][0] == 's') {
    std::thread(leave, argv[1]).join();
  } else {
    HANDLE thread = argv[2][0] == 'n'
                        ? CreateThread(nullptr, 0, native, argv[1], 0, nullptr)
                        : reinterpret_cast<HANDLE>(_beginthreadex(
                              nullptr, 0, ucrt, argv[1], 0, nullptr));
    assert(thread);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  }
  _Exit(2); // Every mode must terminate the process from the worker.
}
