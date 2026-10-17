// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang_wincrt -x c++ -std=c++20 -fno-exceptions %s -x none %t.dll.lib \
// RUN:   -o %t.cpp.exe
// RUN: %run %t.cpp.exe | FileCheck %s
// RUN: llvm-readobj --coff-exports %t.dll | FileCheck --check-prefix=EXPORT %s

// A C program, and a C++ one, read, write and take the address of a C DLL's
// thread-local variable through the record the DLL exports in its place. Each
// thread has its own instance, shared with the DLL's code.

// CHECK: main 7
// CHECK: thread 7
// CHECK: done

// EXPORT-NOT: Name: counter{{$}}
// EXPORT:     Name: _ZTH7counter
// EXPORT:     Name: counter$tls

#include <stdio.h>
#include <windows.h>

#define API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
API extern thread_local int counter;
#else
API extern _Thread_local int counter;
#endif
API int readInDll(void);
API int *addressInDll(void);
#ifdef __cplusplus
}
#endif

#ifdef DLL

_Thread_local int counter = 7;
int readInDll(void) { return counter; }
int *addressInDll(void) { return &counter; }

#else

static DWORD WINAPI thread(LPVOID Arg) {
  (void)Arg;
  printf("thread %d\n", counter);
  fflush(stdout);
  counter = 9;
  return readInDll() == 9 && &counter == addressInDll() ? 0 : 1;
}

int main(void) {
  printf("main %d\n", counter);
  fflush(stdout);
  HANDLE Thread = CreateThread(NULL, 0, thread, NULL, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  DWORD Code = 1;
  GetExitCodeThread(Thread, &Code);
  if (Code != 0 || counter != 7 || &counter != addressInDll())
    return 1;
  printf("done\n");
  return 0;
}

#endif
