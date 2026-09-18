// Nested finalization can free blocks held by an outer filtered traversal.
// RUN: %clangxx_crt_main -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include <assert.h>
#include <windows.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void __cxa_finalize(void *);

static int firstDso, secondDso;
static LONG calls[129];
static void count(void *object) {
  assert(InterlockedIncrement(static_cast<LONG *>(object)) == 1);
}
static void reenter(void *) {
  __cxa_finalize(nullptr);
  assert(__cxa_atexit(count, &calls[128], &firstDso) == 0);
}
static DWORD WINAPI finalize(void *) {
  __cxa_finalize(&firstDso);
  return 0;
}
int main() {
  // Multiple threads must consume each matching entry exactly once.
  for (int i = 0; i < 128; ++i)
    assert(__cxa_atexit(count, &calls[i], &firstDso) == 0);
  HANDLE threads[4];
  for (auto &thread : threads) {
    thread = CreateThread(nullptr, 0, finalize, nullptr, 0, nullptr);
    assert(thread);
  }
  assert(WaitForMultipleObjects(4, threads, TRUE, 5000) == WAIT_OBJECT_0);
  for (auto thread : threads)
    assert(CloseHandle(thread));
  for (int i = 0; i < 128; ++i) {
    assert(calls[i] == 1);
    calls[i] = 0;
    assert(__cxa_atexit(count, &calls[i], i % 2 ? &firstDso : &secondDso) == 0);
  }
  assert(__cxa_atexit(reenter, nullptr, &firstDso) == 0);
  __cxa_finalize(&firstDso);
  __cxa_finalize(nullptr);
  __cxa_finalize(nullptr);
  for (auto value : calls)
    assert(value == 1);
}
