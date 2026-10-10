// REQUIRES: onecoreuap-lib
// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt -static "%onecoreuap_lib" %s %t.dll.lib -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not=api-ms-win-appmodel-runtime

// wincrt's queries of the app model's policies import from kernel32.dll even
// when a library linked before it, such as onecoreuap_apiset.lib, offers the
// same functions from the API set that kernel.appcore.dll hosts. The thread
// start and exit then load neither kernel.appcore.dll nor msvcrt.dll.

#include <process.h>
#include <stdio.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) void loadDll(void) {}

// The detach runs after exit has read the termination policy.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH) {
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
    fflush(stdout);
  }
  return TRUE;
}

#else

__declspec(dllimport) void loadDll(void);

static unsigned __stdcall thread(void *Argument) { return 7; }

int main(void) {
  loadDll();
  HANDLE Thread = (HANDLE)_beginthreadex(NULL, 0, thread, NULL, 0, NULL);
  WaitForSingleObject(Thread, INFINITE);
  DWORD Code;
  GetExitCodeThread(Thread, &Code);
  printf("thread %lu\n", Code);
  return 0;
}

#endif

// CHECK: thread 7
// CHECK: kernel.appcore.dll 0, msvcrt.dll 0

// IMPORT: Name: KERNEL32.dll
// IMPORT: Symbol: AppPolicyGetProcessTerminationMethod
// IMPORT: Symbol: AppPolicyGetThreadInitializationType
