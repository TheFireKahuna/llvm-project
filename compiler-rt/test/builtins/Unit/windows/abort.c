// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %python %S/Inputs/exit_status.py %t.exe noreport | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe dll | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe handler | FileCheck %s \
// RUN:     --check-prefixes=HANDLER,CHECK
// RUN: %python %S/Inputs/exit_status.py %t.exe report | FileCheck %s \
// RUN:     --check-prefix=REPORT
// RUN: %python %S/Inputs/exit_status.py %t.exe handler-report | \
// RUN:     FileCheck %s --check-prefixes=HANDLER,REPORT
// RUN: llvm-readobj --coff-imports %t.exe | \
// RUN:     FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: abort '
// RUN: llvm-readobj --coff-imports %t.dll | \
// RUN:     FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: abort '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %python %S/Inputs/exit_status.py %t-static.exe noreport | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t-static.exe handler | \
// RUN:     FileCheck %s --check-prefixes=HANDLER,CHECK
// RUN: %python %S/Inputs/exit_status.py %t-static.exe report | \
// RUN:     FileCheck %s --check-prefix=REPORT
// RUN: llvm-readobj --coff-imports %t-static.exe | \
// RUN:     FileCheck %s --check-prefix=STATIC \
// RUN:     --implicit-check-not=clang_rt.wincrt_dynamic.dll

// abort behaves as the Universal CRT's: a SIGABRT handler runs first, then
// the process fails fast while _CALL_REPORTFAULT is set, and otherwise ends
// with code 3, without loading kernel.appcore.dll or msvcrt.dll, which the
// Universal CRT's abort loads for it.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) void loadDll(void) {}
__declspec(dllexport) void dllAborts(void) { abort(); }

// The detach runs after abort has ended the process with code 3; failing fast
// runs no detach.
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
__declspec(dllimport) void dllAborts(void);

static void handler(int Signal) {
  printf("handler %d\n", Signal);
  fflush(stdout);
}

int main(int argc, char **argv) {
  loadDll();
  if (strstr(argv[1], "handler"))
    signal(SIGABRT, handler);
  if (!strstr(argv[1], "report") || strstr(argv[1], "noreport"))
    _set_abort_behavior(0, _CALL_REPORTFAULT);
  if (!strcmp(argv[1], "dll"))
    dllAborts();
  abort();
}

#endif

// HANDLER: handler 22
// CHECK: kernel.appcore.dll 0, msvcrt.dll 0
// CHECK: exit code 3

// STATUS_STACK_BUFFER_OVERRUN, the status of a fast fail.
// REPORT-NOT: kernel.appcore.dll
// REPORT: exit code 3221226505

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap_abort

// STATIC: Name: KERNEL32.dll
// STATIC: Symbol: AppPolicyGetProcessTerminationMethod
