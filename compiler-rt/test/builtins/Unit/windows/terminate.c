// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %clang %s %t.dll.lib -o %t.exe
// RUN: %exit_status %t.exe abort | FileCheck %s
// RUN: %exit_status %t.exe dll-abort | FileCheck %s
// RUN: %exit_status %t.exe abort-handler | FileCheck %s --check-prefixes=ABORT,CHECK
// RUN: %exit_status %t.exe abort-report | FileCheck %s --check-prefix=REPORT
// RUN: %exit_status %t.exe abort-handler-report | FileCheck %s --check-prefixes=ABORT,REPORT
// RUN: %exit_status %t.exe assert 2>&1 | FileCheck %s --check-prefixes=WIDE,CHECK
// RUN: %exit_status %t.exe assert-stderr 2>&1 | FileCheck %s --check-prefixes=WIDE,CHECK
// RUN: %exit_status %t.exe _assert 2>&1 | FileCheck %s --check-prefixes=NARROW,CHECK
// RUN: %exit_status %t.exe SIGTERM | FileCheck %s
// RUN: %exit_status %t.exe SIGINT | FileCheck %s
// RUN: %exit_status %t.exe SIGBREAK | FileCheck %s
// RUN: %exit_status %t.exe SIGABRT | FileCheck %s
// RUN: %exit_status %t.exe SIGSEGV | FileCheck %s
// RUN: %exit_status %t.exe SIGFPE | FileCheck %s
// RUN: %exit_status %t.exe dll-raise | FileCheck %s
// RUN: %exit_status %t.exe raise-handler | FileCheck %s --check-prefixes=RAISE,CHECK
// RUN: %exit_status %t.exe raise-ignore | FileCheck %s --check-prefix=IGNORE
// RUN: %exit_status %t.exe raise-invalid | FileCheck %s --check-prefix=INVALID
// RUN: %exit_status %t.exe fault | FileCheck %s --check-prefix=FAULT
// RUN: %exit_status %t.exe purecall | FileCheck %s --check-prefix=PURECALL
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS \
// RUN:   --implicit-check-not='Symbol: _purecall'
// RUN: %clang -static %s %t.dll.lib -o %t-static.exe
// RUN: %exit_status %t-static.exe SIGTERM | FileCheck %s
// RUN: %exit_status %t-static.exe raise-handler | FileCheck %s --check-prefixes=RAISE,CHECK

// abort, a failed assertion and raise's default action end the process as
// the Universal CRT's do, with code 3, from the executable or a DLL, without
// loading kernel.appcore.dll or msvcrt.dll, which the Universal CRT's load for
// them; the DLL's detach shows which are loaded. [ABORT] A SIGABRT handler
// runs first. [REPORT] While _CALL_REPORTFAULT is set abort fails fast, which
// runs no detach. [WIDE] [NARROW] An assertion is reported on standard error
// in a console program or under _OUT_TO_STDERR, by assert and by the narrow
// _assert. [RAISE] A handler runs with the action reset. [IGNORE] SIG_IGN.
// [INVALID] An invalid signal goes to the invalid parameter handler and fails
// with EINVAL. [FAULT] An exception no frame handles reaches the signal
// handlers through the entry point's filter. [PURECALL] [IMPORTS] _purecall
// is the image's own, and it calls the handler of _set_purecall_handler.

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) void dllEnds(int Raise) {
  if (Raise)
    raise(SIGTERM);
  abort();
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH)
    printf("kernel.appcore.dll %d, msvcrt.dll %d\n",
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
  return TRUE;
}

#else

__declspec(dllimport) void dllEnds(int Raise);
// The Universal CRT exports it, but its headers no longer declare it.
void __cdecl _assert(const char *Expression, const char *File, unsigned Line);
int _purecall(void);

static void handler(int Signal) {
  // The Universal CRT resets the action to SIG_DFL before the call.
  printf("handler %d, reset %d\n", Signal, signal(Signal, SIG_GET) == SIG_DFL);
  fflush(stdout);
}
static void quit(int Signal) {
  printf("signal %d\n", Signal);
  fflush(stdout);
  _exit(0);
}
static void quitPurecall(void) { quit(0); }
static void ignoreParameter(const wchar_t *Expression, const wchar_t *Function,
                            const wchar_t *File, unsigned Line, uintptr_t Reserved) {}

static const struct {
  const char *Name;
  int Signal;
} Signals[] = {{"SIGTERM", SIGTERM}, {"SIGINT", SIGINT}, {"SIGBREAK", SIGBREAK},
               {"SIGABRT", SIGABRT}, {"SIGSEGV", SIGSEGV}, {"SIGFPE", SIGFPE}};

int main(int argc, char **argv) {
  const char *Mode = argv[1];
  if ((strstr(Mode, "abort") || strstr(Mode, "assert")) && !strstr(Mode, "report"))
    _set_abort_behavior(0, _CALL_REPORTFAULT);
  if (strstr(Mode, "handler"))
    signal(SIGABRT, handler);
  if (!strncmp(Mode, "abort", 5))
    abort();
  if (!strncmp(Mode, "dll-", 4))
    dllEnds(!strcmp(Mode, "dll-raise"));
  if (!strcmp(Mode, "assert-stderr"))
    _set_error_mode(_OUT_TO_STDERR);
  if (!strncmp(Mode, "assert", 6))
    assert(argc == 0); // WIDE: Assertion failed: argc == 0, file {{.*}}terminate.c, line [[#@LINE]]
  if (!strcmp(Mode, "_assert"))
    _assert("1 + 1 == 3", "narrow.c", 7);
  for (size_t I = 0; I != sizeof(Signals) / sizeof(Signals[0]); ++I)
    if (!strcmp(Mode, Signals[I].Name))
      raise(Signals[I].Signal);
  if (!strncmp(Mode, "raise-", 6)) {
    signal(SIGTERM, !strcmp(Mode, "raise-ignore") ? SIG_IGN : handler);
    _set_invalid_parameter_handler(ignoreParameter);
    errno = 0;
    int Result = raise(!strcmp(Mode, "raise-invalid") ? 99 : SIGTERM);
    printf("raise %d, EINVAL %d\n", Result, errno == EINVAL);
    fflush(stdout);
    if (!strcmp(Mode, "raise-handler"))
      raise(SIGTERM);
    return 0;
  }
  if (!strcmp(Mode, "fault")) {
    signal(SIGSEGV, quit);
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
  }
  if (!strcmp(Mode, "purecall")) {
    _set_purecall_handler(quitPurecall);
    _purecall();
  }
  return 1;
}

#endif

// ABORT:    handler 22
// RAISE:    handler 15, reset 1
// RAISE:    raise 0, EINVAL 0
// NARROW:   Assertion failed: 1 + 1 == 3, file narrow.c, line 7
// CHECK:    kernel.appcore.dll 0, msvcrt.dll 0
// CHECK:    exit code 3
// REPORT-NOT: kernel.appcore.dll
// REPORT:   exit code 3221226505
// IGNORE:   raise 0, EINVAL 0
// IGNORE:   exit code 0
// INVALID:  raise -1, EINVAL 1
// INVALID:  exit code 0
// FAULT:    signal 11
// FAULT:    exit code 0
// PURECALL: signal 0
// PURECALL: exit code 0
// IMPORTS:  Symbol: _get_purecall_handler
