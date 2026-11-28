// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe

// A detach retires its image's record, which is released once no drainer can
// still read it, and a later registration may reuse it. An image that
// registers again at once, as a DLL reloaded at the same base does, while
// other threads drain and release records, keeps every one of its
// registrations: its next detach runs exactly those, in reverse order, across
// the record's first chunks of 64, 128 and 256 entries.

#include <stdio.h>
#include <windows.h>

int __cxa_atexit(void (*)(void *), void *, void *);
int __wincrt_detach_image(void *, int);

enum { Threads = 8, Rounds = 2000, PerRound = 200 };

static char Keys[Threads];
static _Thread_local char Entries[PerRound], *Last;
static _Thread_local int Ran;

static void run(void *Entry) {
  Ran += Last == NULL || (char *)Entry == Last - 1;
  Last = Entry;
}

static DWORD WINAPI thread(LPVOID Key) {
  for (int Round = 0; Round < Rounds; ++Round) {
    for (int I = 0; I < PerRound; ++I)
      if (__cxa_atexit(run, &Entries[I], Key))
        return 1;
    Ran = 0, Last = NULL;
    __wincrt_detach_image(Key, 0);
    if (Ran != PerRound || Last != Entries)
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
    if (Result)
      return printf("thread %d failed: %lu\n", I, Result), 1;
  }
  return 0;
}
