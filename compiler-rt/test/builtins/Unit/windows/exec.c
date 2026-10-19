// RUN: %clang_wincrt -shared -DDLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe
// RUN: %python %S/Inputs/exit_status.py %t.exe _execv | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe _execl | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe _execle | FileCheck %s \
// RUN:     --check-prefixes=CHECK,ENV
// RUN: %python %S/Inputs/exit_status.py %t.exe _wexecv | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe execvp | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe _spawnv | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe _spawnl | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD
// RUN: %python %S/Inputs/exit_status.py %t.exe wait | FileCheck %s \
// RUN:     --check-prefix=WAIT
// RUN: %python %S/Inputs/exit_status.py %t.exe missing | FileCheck %s \
// RUN:     --check-prefix=MISSING
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not='Symbol: _execv ' \
// RUN:     --implicit-check-not='Symbol: _spawnv '
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-static.exe
// RUN: %python %S/Inputs/exit_status.py %t-static.exe _execv | FileCheck %s \
// RUN:     --check-prefixes=CHECK,CHILD

// The exec functions, and the spawn functions in _P_OVERLAY mode, start the
// new process and end this one with code 0, without loading kernel.appcore.dll
// or msvcrt.dll, which the Universal CRT's own _exit loads for them. The
// variadic forms pass their arguments, and the environment forms their
// environment. Every other spawn mode, and a failure to start the process, are
// the Universal CRT's.

#include <direct.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

static const char *Role = "child";

__declspec(dllexport) void setRole(const char *NewRole) { Role = NewRole; }

// The detach runs after the process has ended.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_DETACH) {
    printf("%s: kernel.appcore.dll %d, msvcrt.dll %d\n", Role,
           GetModuleHandleW(L"kernel.appcore.dll") != NULL,
           GetModuleHandleW(L"msvcrt.dll") != NULL);
    fflush(stdout);
  }
  return TRUE;
}

#else

__declspec(dllimport) void setRole(const char *Role);

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "child")) {
    const char *Env = getenv("EXEC_TEST");
    printf("child %s%s%s\n", argv[2], Env ? " " : "", Env ? Env : "");
    fflush(stdout);
    return 0;
  }
  setRole("parent");
  const char *Self = argv[0];
  const char *const Args[] = {Self, "child", "argument", NULL};
  // The child's environment is this one's, which finds the runtime DLLs,
  // with one variable more. The Universal CRT copies the current directory
  // variables (=C: and the like) into an environment it is given, and reads
  // past the end of a process environment that has none, such as one lit
  // gives a test; _chdir sets the current drive's.
  const char *Env[256] = {"EXEC_TEST=environment"};
  for (int I = 0; _environ[I] && I < 254; ++I)
    Env[I + 1] = _environ[I];
  _chdir(".");
  const char *Mode = argv[1];
  if (!strcmp(Mode, "_execv"))
    _execv(Self, Args);
  else if (!strcmp(Mode, "_execl"))
    _execl(Self, Self, "child", "argument", NULL);
  else if (!strcmp(Mode, "_execle"))
    _execle(Self, Self, "child", "argument", NULL, Env);
  else if (!strcmp(Mode, "_wexecv")) {
    wchar_t WideSelf[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, Self, -1, WideSelf, MAX_PATH);
    const wchar_t *const WideArgs[] = {WideSelf, L"child", L"argument", NULL};
    _wexecv(WideSelf, WideArgs);
  } else if (!strcmp(Mode, "execvp"))
    execvp(Self, Args);
  else if (!strcmp(Mode, "_spawnv"))
    _spawnv(_P_OVERLAY, Self, Args);
  else if (!strcmp(Mode, "_spawnl"))
    _spawnl(_P_OVERLAY, Self, Self, "child", "argument", NULL);
  else if (!strcmp(Mode, "wait")) {
    printf("waited %d\n", (int)_spawnv(_P_WAIT, Self, Args));
    fflush(stdout);
    return 5;
  } else if (!strcmp(Mode, "missing")) {
    printf("failed %d\n", (int)_execl("missing.exe", "missing.exe", NULL));
    fflush(stdout);
    return 5;
  }
  return 1;
}

#endif

// CHILD-DAG: child argument{{$}}
// ENV-DAG:   child argument environment
// CHECK-DAG: parent: kernel.appcore.dll 0, msvcrt.dll 0
// CHECK:     exit code 0

// WAIT:      child argument
// WAIT:      waited 0
// WAIT:      exit code 5

// MISSING:      failed -1
// MISSING:      exit code 5

// IMPORT: Name: clang_rt.wincrt_dynamic.dll
// IMPORT: Symbol: __wrap__execv
