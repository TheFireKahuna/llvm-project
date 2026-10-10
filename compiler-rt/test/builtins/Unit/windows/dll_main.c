// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt -shared -DDLL -DNO_DLLMAIN %s -o %t-default.dll
// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe %t.dll | FileCheck %s
// RUN: env WINCRT_FAIL_ATTACH=1 %run %t.exe %t.dll | FileCheck %s --check-prefix=FAIL
// RUN: %run %t.exe %t-default.dll | FileCheck %s --check-prefix=DEFAULT

// _DllMainCRTStartup runs a DLL's constructors before its DllMain, passes
// every notification on to DllMain, and undoes a failed attach once.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#ifdef DLL

static int Constructed;

static void construct(void) { Constructed = 1; }

#  pragma section(".CRT$XCU", read)
__attribute__((used))
__declspec(allocate(".CRT$XCU")) static void (*const Constructor)(void) =
    construct;

__declspec(dllexport) int constructed(void) { return Constructed; }

#  ifndef NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  switch (Reason) {
  case DLL_PROCESS_ATTACH:
    printf("process attach, constructed %d\n", Constructed);
    return !getenv("WINCRT_FAIL_ATTACH");
  case DLL_PROCESS_DETACH:
    printf("process detach\n");
    break;
  case DLL_THREAD_ATTACH:
    printf("thread attach\n");
    break;
  case DLL_THREAD_DETACH:
    printf("thread detach\n");
    break;
  }
  return TRUE;
}
#  endif

#else

static DWORD WINAPI thread(LPVOID Parameter) { return 0; }

int main(int argc, char **argv) {
  HMODULE Module = LoadLibraryA(argv[1]);
  if (!Module) {
    printf("load failed\n");
    return 0;
  }
  int (*Constructed)(void) =
      (int (*)(void))GetProcAddress(Module, "constructed");
  printf("constructed() = %d\n", Constructed());
  HANDLE Thread = CreateThread(NULL, 0, thread, NULL, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  CloseHandle(Thread);
  FreeLibrary(Module);
  printf("unloaded\n");
  return 0;
}

#endif

// Other threads of the process may come and go while the DLL is loaded, so
// the thread notifications are not matched as adjacent lines.
// CHECK:      process attach, constructed 1
// CHECK-NEXT: constructed() = 1
// CHECK:      thread attach
// CHECK:      thread detach
// CHECK:      process detach
// CHECK-NEXT: unloaded

// FAIL:      process attach, constructed 1
// FAIL-NEXT: process detach
// FAIL-NEXT: load failed

// DEFAULT:      constructed() = 1
// DEFAULT-NEXT: unloaded
