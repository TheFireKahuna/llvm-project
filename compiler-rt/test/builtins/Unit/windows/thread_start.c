// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// _beginthread and _beginthreadex start threads as the Universal CRT does: a
// thread's return value or _endthreadex's code is its exit code,
// _beginthread's thread closes its own handle, and the Universal CRT's
// per-thread state works on the thread. rand_s draws varying values, and a
// null argument goes to the invalid parameter handler and fails with EINVAL.
// None of them loads kernel.appcore.dll, advapi32.dll or msvcrt.dll, which the
// Universal CRT's load.

// CHECK:      returns 7, id 1, errno 1234, here 0
// CHECK-NEXT: ends 42
// CHECK-NEXT: _beginthread 1
// CHECK-NEXT: rand_s varied 1
// CHECK-NEXT: invalid parameter
// CHECK-NEXT: rand_s null 1, errno 1
// CHECK-NEXT: kernel.appcore.dll 0, advapi32.dll 0, msvcrt.dll 0

#define _CRT_RAND_S
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
  _endthreadex(42);
  return 0;
}

static void plain(void *Context) { SetEvent(Done); }

static DWORD exitCode(HANDLE Thread) {
  DWORD Code;
  WaitForSingleObject(Thread, INFINITE);
  GetExitCodeThread(Thread, &Code);
  CloseHandle(Thread);
  return Code;
}

static void handler(const wchar_t *Expression, const wchar_t *Function,
                    const wchar_t *File, unsigned Line, uintptr_t Reserved) {
  printf("invalid parameter\n");
}

int main(void) {
  unsigned Id = 0;
  HANDLE T = (HANDLE)_beginthreadex(NULL, 0, returns, (void *)7, 0, &Id);
  printf("returns %lu, id %d, errno %d, here %d\n", exitCode(T), Id != 0, ThreadErrno,
         errno == 1234);
  printf("ends %lu\n", exitCode((HANDLE)_beginthreadex(NULL, 0, ends, NULL, 0, NULL)));
  Done = CreateEventW(NULL, TRUE, FALSE, NULL);
  uintptr_t P = _beginthread(plain, 0, NULL);
  WaitForSingleObject(Done, INFINITE);
  printf("_beginthread %d\n", P != (uintptr_t)-1L);

  unsigned First, Next;
  int Varied = 0;
  if (rand_s(&First))
    return 1;
  for (int I = 0; I < 16 && !Varied; ++I)
    Varied = !rand_s(&Next) && Next != First;
  printf("rand_s varied %d\n", Varied);
  _set_invalid_parameter_handler(handler);
  errno = 0;
  int Error = rand_s(NULL);
  printf("rand_s null %d, errno %d\n", Error == EINVAL, errno == EINVAL);
  printf("kernel.appcore.dll %d, advapi32.dll %d, msvcrt.dll %d\n",
         GetModuleHandleW(L"kernel.appcore.dll") != NULL,
         GetModuleHandleW(L"advapi32.dll") != NULL, GetModuleHandleW(L"msvcrt.dll") != NULL);
  return 0;
}
