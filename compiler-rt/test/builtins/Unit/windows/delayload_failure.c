// The executables import two functions, and the DLL they then load defines
// only one of them.
// RUN: %clang_wincrt -shared -DBUILD_DLL -DBOTH %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clang_wincrt -DFAILURE_HOOK %s %t.dll.lib -o %t-hook.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clang_wincrt -mguard=cf -DFAILURE_HOOK %s %t.dll.lib \
// RUN:   -o %t-protected-hook.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clang_wincrt -mguard=cf -DFAILURE_HOOK %s %t.dll.lib \
// RUN:   -o %t-unload-hook.exe -Wl,/delayload:%basename_t.tmp.dll \
// RUN:   -Wl,/delay:unload
// RUN: %clang_wincrt %s %t.dll.lib -o %t-unload.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll -Wl,/delay:unload
// The loader then searches only System32 for every DLL the executable loads,
// so it takes the termination registries from -static.
// RUN: %clang_wincrt -static %s %t.dll.lib -o %t-system32.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll -Wl,/dependentloadflag:0x800
// RUN: %clang_wincrt -shared -DBUILD_DLL %s -o %t.dll

// RUN: %run %t.exe 2>&1 | FileCheck %s --check-prefix=PROC
// RUN: %run %t-unload.exe 2>&1 | FileCheck %s --check-prefix=PROC
// RUN: %run %t.exe present-only 2>&1 | FileCheck %s --check-prefix=PRESENT
// RUN: %run %t-unload.exe present-only 2>&1 \
// RUN:   | FileCheck %s --check-prefix=PRESENT
// RUN: %run %t-system32.exe 2>&1 | FileCheck %s --check-prefix=MOD
// RUN: %run %t-hook.exe 2>&1 | FileCheck %s --check-prefix=LOADER-HOOK
// RUN: %run %t-protected-hook.exe 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LOADER-HOOK
// RUN: %run %t-unload-hook.exe 2>&1 | FileCheck %s --check-prefix=IMAGE-HOOK
// RUN: %run %t.exe load-all %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload.exe load-all %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload-hook.exe load-all %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LOAD-ALL-HOOK

// A delayed import that cannot be resolved is reported as delayimp.h
// specifies: the image's failure hook is asked first, and otherwise a
// structured exception carrying a DelayLoadInfo is raised, rather than the
// helper returning a null address for the thunk to jump to. The search path
// is the one /dependentloadflag names: restricted to System32, the DLL beside
// the executable is not found. The failure hook's answer is stored only when
// the image resolves the import itself; on the loader's path each call asks
// again. An import that is never called never fails, even when the image
// resolves the library's other imports together. __HrLoadAllImportsForDll
// reports a failure as a call would, on either path.

#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef BUILD_DLL
__declspec(dllexport) int present(int Value) { return Value + 1; }
#  ifdef BOTH
__declspec(dllexport) int missing(int Value) { return Value; }
#  endif
#else

__declspec(dllimport) int present(int);
__declspec(dllimport) int missing(int);

// The delayimp.h ABI.
typedef struct DelayLoadInfo {
  DWORD cb;
  const void *pidd;
  FARPROC *ppfn;
  LPCSTR szDll;
  struct {
    BOOL fImportByName;
    union {
      LPCSTR szProcName;
      DWORD dwOrdinal;
    };
  } dlp;
  HMODULE hmodCur;
  FARPROC pfnCur;
  DWORD dwLastError;
} DelayLoadInfo;

#  define VCPP_EXCEPTION(Error)                                                \
    (ERROR_SEVERITY_ERROR | FACILITY_VISUALCPP << 16 | (Error))

#  ifdef FAILURE_HOOK
static int substitute(int Value) { return -Value; }

static FARPROC WINAPI failureHook(unsigned Reason, DelayLoadInfo *Info) {
  fprintf(stderr, "hook %u %s\n", Reason, Info->dlp.szProcName);
  return Reason == 4 ? (FARPROC)substitute : NULL;
}
FARPROC(WINAPI *const __pfnDliFailureHook2)(unsigned,
                                            DelayLoadInfo *) = failureHook;
#  endif

static DelayLoadInfo Captured;

static int filter(DWORD Code, EXCEPTION_POINTERS *Pointers) {
  if (Code != VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND) &&
      Code != VCPP_EXCEPTION(ERROR_PROC_NOT_FOUND))
    return EXCEPTION_CONTINUE_SEARCH;
  Captured =
      *(DelayLoadInfo *)Pointers->ExceptionRecord->ExceptionInformation[0];
  return EXCEPTION_EXECUTE_HANDLER;
}

static void call(const char *Name, int (*Function)(int)) {
  __try {
    fprintf(stderr, "%s(2) = %d\n", Name, Function(2));
  } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
    fprintf(stderr, "caught %s %s %s %lu\n",
            (DWORD)GetExceptionCode() == VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND)
                ? "module"
                : "procedure",
            Captured.dlp.szProcName, Captured.szDll ? "dll" : "none",
            Captured.cb);
  }
}

HRESULT WINAPI __HrLoadAllImportsForDll(LPCSTR);

static void loadAll(const char *Dll) {
  __try {
    fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(Dll));
  } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
    fprintf(stderr, "load all caught %s\n", Captured.dlp.szProcName);
  }
}

int main(int argc, char **argv) {
  if (argc > 2 && strcmp(argv[1], "load-all") == 0) {
    loadAll(argv[2]);
    call("present", present);
    return 0;
  }
  call("present", present);
  if (argc > 1 && strcmp(argv[1], "present-only") == 0) {
    call("present", present);
    return 0;
  }
  call("missing", missing);
  call("missing", missing);
  return 0;
}
#endif

// PROC: present(2) = 3
// PROC-NEXT: caught procedure missing dll 72
// PROC-NEXT: caught procedure missing dll 72

// PRESENT:      present(2) = 3
// PRESENT-NEXT: present(2) = 3
// PRESENT-NOT:  {{.}}

// MOD: caught module present dll 72
// MOD-NEXT: caught module missing dll 72
// MOD-NEXT: caught module missing dll 72

// LOADER-HOOK: present(2) = 3
// LOADER-HOOK-NEXT: hook 4 missing
// LOADER-HOOK-NEXT: missing(2) = -2
// LOADER-HOOK-NEXT: hook 4 missing
// LOADER-HOOK-NEXT: missing(2) = -2

// IMAGE-HOOK: present(2) = 3
// IMAGE-HOOK-NEXT: hook 4 missing
// IMAGE-HOOK-NEXT: missing(2) = -2
// IMAGE-HOOK-NEXT: missing(2) = -2

// LOAD-ALL:      load all caught missing
// LOAD-ALL-NEXT: present(2) = 3

// LOAD-ALL-HOOK:      hook 4 missing
// LOAD-ALL-HOOK-NEXT: load all 0
// LOAD-ALL-HOOK-NEXT: present(2) = 3
