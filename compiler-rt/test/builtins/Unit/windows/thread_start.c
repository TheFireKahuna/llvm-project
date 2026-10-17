// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT
// RUN: %clang_wincrt -static %s -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s

// _beginthread and _beginthreadex start threads as the Universal CRT does,
// without loading kernel.appcore.dll or msvcrt.dll: a thread's return value
// or _endthreadex's code is its exit code, _beginthread's thread closes its
// own handle, and the Universal CRT's per-thread state works on the thread.

#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static HANDLE Done;
static int ThreadErrno;

static unsigned __stdcall returns(void *Context) {
  errno = 1234;
  ThreadErrno = errno;
  return (unsigned)(uintptr_t)Context;
}

static unsigned __stdcall ends(void *Context) {
  (void)Context;
  _endthreadex(42);
  return 0;
}

static void plain(void *Context) {
  (void)Context;
  SetEvent(Done);
}

static DWORD exitCode(HANDLE Thread) {
  DWORD Code;
  WaitForSingleObject(Thread, INFINITE);
  GetExitCodeThread(Thread, &Code);
  CloseHandle(Thread);
  return Code;
}

int main(void) {
  unsigned Id = 0;
  HANDLE T = (HANDLE)_beginthreadex(NULL, 0, returns, (void *)7, 0, &Id);
  printf("returns %lu, id %d, errno %d, here %d\n", exitCode(T), Id != 0,
         ThreadErrno, errno == 1234);
  printf("ends %lu\n", exitCode((HANDLE)_beginthreadex(NULL, 0, ends, NULL,
                                                       0, NULL)));
  Done = CreateEventW(NULL, TRUE, FALSE, NULL);
  uintptr_t P = _beginthread(plain, 0, NULL);
  WaitForSingleObject(Done, INFINITE);
  printf("_beginthread %d\n", P != (uintptr_t)-1L);
  printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
         GetModuleHandleW(L"kernel.appcore.dll") != NULL,
         GetModuleHandleW(L"msvcrt.dll") != NULL);
  return 0;
}

// CHECK:      returns 7, id 1, errno 1234, here 0
// CHECK-NEXT: ends 42
// CHECK-NEXT: _beginthread 1
// CHECK-NEXT: kernel.appcore.dll 0, msvcrt.dll 0

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap__beginthread
// IMPORT: Symbol: __wrap__beginthreadex
// IMPORT: Symbol: __wrap__endthreadex
