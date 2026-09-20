// A delayed import that cannot be resolved reports itself the way delayimp.h
// specifies, as a structured exception carrying a DelayLoadInfo, instead of
// returning a null address for the calling stub to jump to. The image's own
// failure hook is asked first and may supply a substitute.
// RUN: %clangxx_crt_dll -O2 -DBUILD_DLL %s -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -O2 %s %t.lib -o %t.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clangxx_crt_main -O2 -DUSE_HOOK %s %t.lib -o %t.hook.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: rm %t.dll
// RUN: %run %t.exe 2>&1 | FileCheck %s --check-prefix=RAISE
// RUN: %run %t.hook.exe 2>&1 | FileCheck %s --check-prefix=HOOK
// REQUIRES: windows, crt

#include <stdio.h>

#if defined(BUILD_DLL)
extern "C" __declspec(dllexport) int delayed(int value) { return value * 2; }
#else
#  define WIN32_LEAN_AND_MEAN
#  include <assert.h>
#  include <windows.h>

extern "C" __declspec(dllimport) int delayed(int);

// delayimp.h ABI, spelled out because that header ships only with MSVC.
struct DelayLoadInfo {
  DWORD cb;
  const void *pidd;
  void **ppfn;
  LPCSTR szDll;
  struct {
    BOOL fImportByName;
    union {
      LPCSTR szProcName;
      DWORD dwOrdinal;
    };
  } dlp;
  HMODULE hmodCur;
  void *pfnCur;
  DWORD dwLastError;
};

#  define VcppException(sev, err) ((sev) | (FACILITY_VISUALCPP << 16) | (err))

#  if defined(USE_HOOK)
static void *__stdcall failureHook(unsigned reason, DelayLoadInfo *info) {
  // 3 is dliFailLoadLib: the DLL was the thing that could not be found.
  fprintf(stderr, "hook %u %s %s\n", reason, info->szDll,
          info->dlp.szProcName);
  return nullptr;
}
extern "C" const void *__pfnDliFailureHook2 = (const void *)failureHook;
#  endif

static DelayLoadInfo captured;

static int filter(DWORD code, EXCEPTION_POINTERS *pointers) {
  if (code != VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND))
    return EXCEPTION_CONTINUE_SEARCH;
  auto *info = reinterpret_cast<DelayLoadInfo *>(
      pointers->ExceptionRecord->ExceptionInformation[0]);
  captured = *info;
  return EXCEPTION_EXECUTE_HANDLER;
}

int main() {
  int result = -1;
  __try {
    result = delayed(21);
  } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
    fprintf(stderr, "caught %s %s %lu\n", captured.szDll,
            captured.dlp.szProcName, captured.cb);
    return 0;
  }
  fprintf(stderr, "no exception, delayed(21) = %d\n", result);
  return 1;
}
#endif

// RAISE: caught {{.*}}.dll delayed 72

// The hook sees the load failure first, and declining it raises as before.
// HOOK: hook 3 {{.*}}.dll delayed
// HOOK-NEXT: caught {{.*}}.dll delayed 72
