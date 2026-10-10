// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %run %t.exe return | FileCheck %s
// RUN: %run %t.exe exit | FileCheck %s
// RUN: %run %t.exe _exit | FileCheck %s
// RUN: %run %t.exe dll | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: exit ' \
// RUN:     --implicit-check-not='Symbol: _exit '
// RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: exit '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %run %t-static.exe exit | FileCheck %s
// RUN: %run %t-static.exe dll | FileCheck %s
// RUN: llvm-readobj --coff-imports %t-static.exe | \
// RUN:     FileCheck %s --check-prefix=STATIC \
// RUN:     --implicit-check-not=clang_rt.wincrt_dynamic.dll \
// RUN:     --implicit-check-not='Symbol: exit ' \
// RUN:     --implicit-check-not='Symbol: _exit '

// exit, called by the executable or a DLL, _exit and the return from main
// read the app model's termination policy without loading kernel.appcore.dll
// or msvcrt.dll, which the Universal CRT's exit loads for it. Every image's
// references reach wincrt's exit, in clang_rt.wincrt_dynamic.dll or, with
// -static, in the executable, and none imports the Universal CRT's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) void loadDll(void) {}
__declspec(dllexport) void dllCallsExit(void) { exit(0); }

// The detach runs after exit has read the policy.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH)
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
  return TRUE;
}

#else

__declspec(dllimport) void loadDll(void);
__declspec(dllimport) void dllCallsExit(void);

int main(int argc, char **argv) {
  loadDll();
  if (!strcmp(argv[1], "dll"))
    dllCallsExit();
  if (!strcmp(argv[1], "exit"))
    exit(0);
  if (!strcmp(argv[1], "_exit")) {
    fflush(stdout);
    _exit(0);
  }
  return 0;
}

#endif

// CHECK: kernel.appcore.dll 0, msvcrt.dll 0

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap_exit

// STATIC: Name: KERNEL32.dll
// STATIC: Symbol: AppPolicyGetProcessTerminationMethod
