// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGTERM | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGINT | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGBREAK | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGABRT | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGSEGV | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe SIGFPE | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe dll | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t.exe handler | FileCheck %s \
// RUN:     --check-prefixes=HANDLER,CHECK
// RUN: %python %S/Inputs/exit_status.py %t.exe ignore | FileCheck %s \
// RUN:     --check-prefix=IGNORE
// RUN: %python %S/Inputs/exit_status.py %t.exe invalid | FileCheck %s \
// RUN:     --check-prefix=INVALID
// RUN: llvm-readobj --coff-imports %t.exe | \
// RUN:     FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: raise '
// RUN: llvm-readobj --coff-imports %t.dll | \
// RUN:     FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: raise '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %python %S/Inputs/exit_status.py %t-static.exe SIGTERM | FileCheck %s
// RUN: %python %S/Inputs/exit_status.py %t-static.exe handler | FileCheck %s \
// RUN:     --check-prefixes=HANDLER,CHECK
// RUN: llvm-readobj --coff-imports %t-static.exe | \
// RUN:     FileCheck %s --check-prefix=STATIC \
// RUN:     --implicit-check-not=clang_rt.wincrt_dynamic.dll

// raise takes a signal's default action as the Universal CRT does, ending the
// process with code 3, without loading kernel.appcore.dll or msvcrt.dll, which
// the Universal CRT's raise loads for it. A handler, SIG_IGN and an invalid
// signal behave as with the Universal CRT's raise.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) void loadDll(void) {}
__declspec(dllexport) void dllRaises(void) { raise(SIGTERM); }

// The detach runs after the default action has ended the process.
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
__declspec(dllimport) void dllRaises(void);

static void handler(int Signal) {
  // The Universal CRT resets the action to SIG_DFL before the call.
  printf("handler %d, reset %d\n", Signal,
         signal(Signal, SIG_GET) == SIG_DFL);
  fflush(stdout);
}

static void ignoreParameter(const wchar_t *Expression, const wchar_t *Function,
                            const wchar_t *File, unsigned Line,
                            uintptr_t Reserved) {}

static const struct {
  const char *Name;
  int Signal;
} Signals[] = {{"SIGTERM", SIGTERM}, {"SIGINT", SIGINT},
               {"SIGBREAK", SIGBREAK}, {"SIGABRT", SIGABRT},
               {"SIGSEGV", SIGSEGV}, {"SIGFPE", SIGFPE}};

int main(int argc, char **argv) {
  loadDll();
  for (size_t I = 0; I != sizeof(Signals) / sizeof(Signals[0]); ++I)
    if (!strcmp(argv[1], Signals[I].Name))
      raise(Signals[I].Signal);
  if (!strcmp(argv[1], "dll"))
    dllRaises();
  if (!strcmp(argv[1], "handler")) {
    signal(SIGTERM, handler);
    printf("raise %d\n", raise(SIGTERM));
    fflush(stdout);
    raise(SIGTERM);
  }
  if (!strcmp(argv[1], "ignore")) {
    signal(SIGTERM, SIG_IGN);
    printf("raise %d\n", raise(SIGTERM));
    fflush(stdout);
    return 0;
  }
  if (!strcmp(argv[1], "invalid")) {
    _set_invalid_parameter_handler(ignoreParameter);
    int Result = raise(99);
    printf("raise %d, EINVAL %d\n", Result, errno == EINVAL);
    fflush(stdout);
    return 0;
  }
  return 1;
}

#endif

// HANDLER: handler 15, reset 1
// HANDLER: raise 0
// CHECK: kernel.appcore.dll 0, msvcrt.dll 0
// CHECK: exit code 3

// IGNORE: raise 0
// IGNORE: exit code 0

// INVALID: raise -1, EINVAL 1
// INVALID: exit code 0

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap_raise

// STATIC: Name: KERNEL32.dll
// STATIC: Symbol: AppPolicyGetProcessTerminationMethod
