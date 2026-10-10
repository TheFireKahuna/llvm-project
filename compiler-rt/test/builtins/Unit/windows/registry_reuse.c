// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe

// A detach releases its image's record once no other thread is draining, and
// a later registration may reuse it. An image that registers again at once,
// as a DLL reloaded at the same base does, while other threads drain and
// release records, keeps every one of its registrations: its next detach
// runs exactly those.

#include <stdio.h>
#include <windows.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int __wincrt_detach_image(void *, int);

enum { Threads = 8, Rounds = 20000, PerRound = 4 };

static char Keys[Threads];

static void count(void *Counter) { ++*(volatile long *)Counter; }

static DWORD WINAPI thread(LPVOID Parameter) {
  char *Key = Parameter;
  long Counter = 0;
  for (int Round = 0; Round < Rounds; ++Round) {
    for (int I = 0; I < PerRound; ++I)
      if (__cxa_atexit(count, &Counter, Key))
        return 1;
    __wincrt_detach_image(Key, 0);
    if (Counter != (long)(Round + 1) * PerRound)
      return 2;
  }
  return 0;
}

int main(void) {
  HANDLE Handles[Threads];
  for (int I = 0; I < Threads; ++I)
    Handles[I] = CreateThread(NULL, 0, thread, &Keys[I], 0, NULL);
  WaitForMultipleObjects(Threads, Handles, TRUE, INFINITE);
  for (int I = 0; I < Threads; ++I) {
    DWORD Result;
    GetExitCodeThread(Handles[I], &Result);
    if (Result) {
      printf("thread %d failed: %lu\n", I, Result);
      return 1;
    }
  }
  return 0;
}
