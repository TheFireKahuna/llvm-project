// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %python %S/Inputs/exit_status.py %t.exe wide 2>&1 | FileCheck %s \
// RUN:     --check-prefixes=CHECK,WIDE
// RUN: %python %S/Inputs/exit_status.py %t.exe narrow 2>&1 | FileCheck %s \
// RUN:     --check-prefixes=CHECK,NARROW
// RUN: %python %S/Inputs/exit_status.py %t.exe stderr 2>&1 | FileCheck %s \
// RUN:     --check-prefixes=CHECK,WIDE
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: _wassert '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %python %S/Inputs/exit_status.py %t-static.exe wide 2>&1 | \
// RUN:     FileCheck %s --check-prefixes=CHECK,WIDE

// A failed assertion in a console program, or under _OUT_TO_STDERR, is
// reported on standard error and aborts. With _CALL_REPORTFAULT cleared the
// abort ends the program with code 3, without loading kernel.appcore.dll or
// msvcrt.dll, which the Universal CRT's own abort loads for an assertion.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

// The detach runs after the program has ended.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH) {
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
    fflush(stdout);
  }
  return TRUE;
}

__declspec(dllexport) void loadDll(void) {}

#else

__declspec(dllimport) void loadDll(void);
// The narrow form, which the Universal CRT exports but its headers no longer
// declare.
void __cdecl _assert(const char *Expression, const char *File, unsigned Line);

int main(int argc, char **argv) {
  loadDll();
  _set_abort_behavior(0, _CALL_REPORTFAULT);
  if (!strcmp(argv[1], "narrow"))
    _assert("1 + 1 == 3", "narrow.c", 7);
  if (!strcmp(argv[1], "stderr"))
    _set_error_mode(_OUT_TO_STDERR);
  assert(argc == 0);
  return 0;
}

#endif

// WIDE:   Assertion failed: argc == 0, file {{.*}}assert.c, line 55
// NARROW: Assertion failed: 1 + 1 == 3, file narrow.c, line 7
// CHECK:  kernel.appcore.dll 0, msvcrt.dll 0
// CHECK:  exit code 3

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap__wassert
