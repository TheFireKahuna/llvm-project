// RUN: %clang %s -o %t.exe
// RUN: llvm-readobj --file-headers %t.exe | FileCheck %s
// RUN: %run %t.exe

// A program with thread-local variables gets a TLS directory, each thread its
// own copy of their initial values, and a callback in .CRT$XL the loader's
// notifications.

#include <windows.h>

static _Thread_local int Value = 42;
static int Attached;

static void NTAPI callback(void *Module, DWORD Reason, void *Reserved) {
  if (Reason == DLL_PROCESS_ATTACH)
    Attached = Value;
}

__attribute__((used, section(".CRT$XLB"))) static const PIMAGE_TLS_CALLBACK C = callback;

static DWORD WINAPI thread(LPVOID Parameter) {
  (void)Parameter;
  if (Value != 42)
    return 1;
  Value = 7;
  return 0;
}

int main(void) {
  Value = 1;
  HANDLE Thread = CreateThread(NULL, 0, thread, NULL, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  DWORD Result;
  GetExitCodeThread(Thread, &Result);
  CloseHandle(Thread);
  return Result || Value != 1 || Attached != 42;
}

// CHECK: TLSTableSize: 0x28
