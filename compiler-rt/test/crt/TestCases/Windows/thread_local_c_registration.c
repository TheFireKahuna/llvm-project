// The local ABI fallback must also work without linking the shared C++ runtime.
// RUN: %clang_crt_main -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// REQUIRES: windows, crt

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <windows.h>

extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void __cxa_thread_finalize(void *);
static LONG destroyed;
static void destroy(void *object) { InterlockedIncrement((LONG *)object); }
static DWORD WINAPI worker(void *unused) {
  (void)unused;
  return __cxa_thread_atexit_impl(destroy, &destroyed, NULL);
}
int main(void) {
  HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  assert(thread);
  assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  DWORD code;
  assert(GetExitCodeThread(thread, &code) && code == 0);
  assert(destroyed == 1);
  assert(CloseHandle(thread));
  assert(__cxa_thread_atexit_impl(destroy, &destroyed, NULL) == 0);
  __cxa_thread_finalize(NULL);
  assert(destroyed == 2);
  __cxa_thread_finalize(NULL);
  assert(destroyed == 2);
}
