// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %clang %s %t.dll.lib -o %t.exe
// RUN: %clang -static %s %t.dll.lib -o %t-static.exe
// RUN: llvm-readobj --file-headers %t.exe | FileCheck %s
// RUN: %run %t.exe
// RUN: %run %t-static.exe

// A program with thread-local variables gets a TLS directory, each thread its
// own copy of their initial values, and a callback in .CRT$XL the loader's
// notifications. A DLL's initializer that stores to the executable's
// thread-local before the executable's first callback runs keeps its store,
// under -static too, where that callback founds the runtime.

#include <windows.h>

#ifdef DLL
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  void (*Attach)(void) =
      (void (*)(void))GetProcAddress(GetModuleHandleW(NULL), "exe_attach");
  if (Reason == DLL_PROCESS_ATTACH && Attach)
    Attach();
  return TRUE;
}
__declspec(dllexport) int dll_value(void) { return 0; }
#else
__declspec(dllimport) int dll_value(void);

static _Thread_local int Value = 42;
static int Attached;

__declspec(dllexport) void exe_attach(void) { Value = 5; }

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
  return Result || Value != 1 || Attached != 5 || dll_value();
}
#endif

// CHECK: TLSTableSize: 0x28
