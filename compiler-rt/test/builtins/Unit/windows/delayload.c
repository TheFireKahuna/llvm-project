// DEFINE: %{link} = %clang %s %t.dll.lib -Wl,/delayload:%basename_t.tmp.dll
// RUN: %clang -shared -DDLL -DMISSING %s -o %t.dll
// RUN: %{link} -mguard=none -o %t.exe
// RUN: %{link} -o %t-protected.exe
// RUN: %{link} -DWHOLE_SECTION -o %t-whole.exe
// RUN: %{link} -mguard=none -DNOTIFY_HOOK -o %t-hook.exe
// RUN: %{link} -mguard=none -Xlinker -subsystem:console,10.0 -DNOTIFY_HOOK -o %t-protected-hook.exe
// RUN: %{link} -mguard=none -DFAILURE_HOOK -o %t-failure-hook.exe
// RUN: %{link} -DFAILURE_HOOK -o %t-protected-failure-hook.exe
// RUN: %{link} -DFAILURE_HOOK -Wl,/delay:unload -o %t-unload-failure-hook.exe
// RUN: %{link} -mguard=none -Wl,/delay:unload -o %t-unload.exe
// RUN: %{link} -Wl,/delay:unload -o %t-protected-unload.exe
// The loader then searches only System32 for every DLL the executable loads,
// so it takes the termination registries from -static.
// RUN: %{link} -static -Wl,/dependentloadflag:0x800 -o %t-system32.exe
// RUN: %run %t.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s
// RUN: %run %t-protected.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,NARROW
// RUN: %run %t-whole.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,WHOLE
// RUN: %run %t-hook.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,HOOK
// RUN: %run %t-protected-hook.exe calls %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefixes=CHECK,PROTECTED,NARROW,HOOK
// The DLL loses the export the executables import as missing.
// RUN: %clang -shared -DDLL %s -o %t.dll
// RUN: %run %t.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,PROC
// RUN: %run %t-unload.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,PROC
// RUN: %run %t-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK,LOADER
// RUN: %run %t-protected-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK,LOADER
// RUN: %run %t-unload-failure-hook.exe missing 2>&1 | FileCheck %s --check-prefixes=MISSING,FHOOK
// RUN: %run %t-system32.exe missing 2>&1 | FileCheck %s --check-prefix=MODULE
// RUN: %run %t.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL
// RUN: %run %t-unload-failure-hook.exe load-all %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=LOAD-ALL-HOOK
// RUN: %run %t-unload.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t-protected-unload.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=UNLOAD
// RUN: %run %t-protected.exe unload %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=NO-UNLOAD

// [CHECK] A delayed import resolves on its first call, through the loader or,
// with a delayimp.h notify hook installed, in the image. [HOOK] The hook sees
// every stage of each import as it is called. [PROTECTED] Under Control Flow
// Guard, the default, or for a subsystem version above 6.3, since the helper's
// object defines __MSSafeDelayLoadIAT, the table is read-only before and after
// it is written. [NARROW] [WHOLE] A program's choice to make the whole section
// writable meanwhile is read as false when it makes none, and has no effect.
// [PROC] An import that cannot be resolved raises a structured exception
// carrying a DelayLoadInfo, rather than the helper returning a null address,
// and one never called never fails. [FHOOK] The failure hook is asked first;
// [LOADER] its answer is stored only when the image resolves the import
// itself. [MODULE] /dependentloadflag names the search path; restricted to
// System32, the DLL beside the executable is not found. [LOAD-ALL]
// __HrLoadAllImportsForDll reports a failure as a call does, on either path.
// [UNLOAD] __FUnloadDelayLoadedDLL2 restores the table and frees the library,
// so the next call loads it again, matching the name without regard to case
// and refusing a library the image does not delay-load. [NO-UNLOAD] Without
// the copy of the table /delay:unload makes, it refuses.

// PROTECTED:      read-only before 1
// NARROW-NEXT:    whole section 0
// WHOLE-NEXT:     whole section 1
// HOOK:           hook start twice
// HOOK-NEXT:      hook pre-load twice
// HOOK-NEXT:      hook pre-getproc twice
// HOOK-NEXT:      hook end twice
// CHECK:          twice(21) = 42
// HOOK-NEXT:      hook start add
// HOOK-NEXT:      hook pre-getproc add
// HOOK-NEXT:      hook end add
// CHECK:          add(1, 2) = 3
// HOOK-NEXT:      hook start missing
// HOOK-NEXT:      hook pre-getproc missing
// HOOK-NEXT:      hook end missing
// CHECK-NEXT:     load all 0
// CHECK-NEXT:     load all of another not found
// PROTECTED-NEXT: read-only after 1

// MISSING:       twice(2) = 4
// MISSING-NEXT:  twice(2) = 4
// PROC-NEXT:     caught procedure missing dll 1
// PROC-NEXT:     caught procedure missing dll 1
// FHOOK-NEXT:    hook 4 missing
// FHOOK-NEXT:    missing(2) = -2
// LOADER-NEXT:   hook 4 missing
// FHOOK-NEXT:    missing(2) = -2

// MODULE:      caught module twice dll 1
// MODULE-NEXT: caught module twice dll 1
// MODULE-NEXT: caught module missing dll 1
// MODULE-NEXT: caught module missing dll 1

// LOAD-ALL:           load all caught missing
// LOAD-ALL-NEXT:      twice(2) = 4
// LOAD-ALL-HOOK:      hook 4 missing
// LOAD-ALL-HOOK-NEXT: load all 0
// LOAD-ALL-HOOK-NEXT: twice(2) = 4

// UNLOAD:         unloaded 1, still loaded 0
// UNLOAD-NEXT:    twice(4) = 8, loaded 1
// NO-UNLOAD:      unloaded 0, still loaded 1
// NO-UNLOAD-NOT:  twice

#include <delayimp.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef DLL
__declspec(dllexport) int twice(int Value) { return 2 * Value; }
__declspec(dllexport) int add(int A, int B) { return A + B; }
#  ifdef MISSING
__declspec(dllexport) int missing(int Value) { return Value; }
#  endif
#else

__declspec(dllimport) int twice(int);
__declspec(dllimport) int add(int, int);
__declspec(dllimport) int missing(int);
extern IMAGE_DOS_HEADER __ImageBase;

#  ifdef WHOLE_SECTION
const BOOL __bChangeProtectionOfWholeDloadSection = TRUE;
#  endif

#  ifdef NOTIFY_HOOK
static FARPROC WINAPI notifyHook(unsigned Reason, DelayLoadInfo *Info) {
  static const char *const Stages[] = {"start", "pre-load", "pre-getproc",
                                       "fail-load", "fail-getproc", "end"};
  fprintf(stderr, "hook %s %s\n", Stages[Reason], Info->dlp.szProcName);
  return NULL;
}
const PfnDliHook __pfnDliNotifyHook2 = notifyHook;
#  endif

#  ifdef FAILURE_HOOK
static int substitute(int Value) { return -Value; }
static FARPROC WINAPI failureHook(unsigned Reason, DelayLoadInfo *Info) {
  fprintf(stderr, "hook %u %s\n", Reason, Info->dlp.szProcName);
  return Reason == dliFailGetProc ? (FARPROC)substitute : NULL;
}
const PfnDliHook __pfnDliFailureHook2 = failureHook;
#  endif

// Whether the page of the first descriptor's address table is read-only.
static int tableIsReadOnly(void) {
  const char *Base = (const char *)&__ImageBase;
  const IMAGE_NT_HEADERS64 *Nt = (const IMAGE_NT_HEADERS64 *)(Base + __ImageBase.e_lfanew);
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor =
      (const IMAGE_DELAYLOAD_DESCRIPTOR *)(Base + Nt->OptionalHeader.DataDirectory
                                                      [IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                                                          .VirtualAddress);
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Base + Descriptor->ImportAddressTableRVA, &Info, sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}

#  define VCPP_EXCEPTION(Error) VcppException(ERROR_SEVERITY_ERROR, Error)
static DelayLoadInfo Captured;

static int filter(DWORD Code, EXCEPTION_POINTERS *Pointers) {
  if (Code != VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND) && Code != VCPP_EXCEPTION(ERROR_PROC_NOT_FOUND))
    return EXCEPTION_CONTINUE_SEARCH;
  Captured = *(DelayLoadInfo *)Pointers->ExceptionRecord->ExceptionInformation[0];
  return EXCEPTION_EXECUTE_HANDLER;
}

static void call(const char *Name, int (*Function)(int)) {
  __try {
    fprintf(stderr, "%s(2) = %d\n", Name, Function(2));
  } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
    fprintf(stderr, "caught %s %s %s %d\n",
            (DWORD)GetExceptionCode() == VCPP_EXCEPTION(ERROR_MOD_NOT_FOUND) ? "module"
                                                                           : "procedure",
            Captured.dlp.szProcName, Captured.szDll ? "dll" : "none",
            Captured.cb == sizeof(DelayLoadInfo));
  }
}

int main(int argc, char **argv) {
  const char *Mode = argv[1], *Dll = argv[2];
  if (!strcmp(Mode, "calls")) {
    if (GetModuleHandleA(Dll))
      return 1;
    fprintf(stderr, "read-only before %d\n", tableIsReadOnly());
    fprintf(stderr, "whole section %d\n", __bChangeProtectionOfWholeDloadSection);
    fprintf(stderr, "twice(21) = %d\n", twice(21));
    if (!GetModuleHandleA(Dll))
      return 2;
    fprintf(stderr, "add(1, 2) = %d\n", add(1, 2));
    fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(Dll));
    fprintf(stderr, "load all of another %s\n",
            __HrLoadAllImportsForDll("no-such-library.dll") == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
                ? "not found"
                : "found");
    fprintf(stderr, "read-only after %d\n", tableIsReadOnly());
  }
  if (!strcmp(Mode, "missing")) {
    call("twice", twice);
    call("twice", twice);
    call("missing", missing);
    call("missing", missing);
  }
  if (!strcmp(Mode, "load-all")) {
    __try {
      fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(Dll));
    } __except (filter(GetExceptionCode(), GetExceptionInformation())) {
      fprintf(stderr, "load all caught %s\n", Captured.dlp.szProcName);
    }
    call("twice", twice);
  }
  if (!strcmp(Mode, "unload")) {
    if (GetModuleHandleA(Dll) || twice(21) != 42 || !GetModuleHandleA(Dll) ||
        __FUnloadDelayLoadedDLL2("no-such-library.dll"))
      return 3;
    char Name[MAX_PATH];
    CharUpperA(strcpy(Name, Dll));
    BOOL Unloaded = __FUnloadDelayLoadedDLL2(Name);
    fprintf(stderr, "unloaded %d, still loaded %d\n", Unloaded, GetModuleHandleA(Dll) != NULL);
    if (Unloaded && __FUnloadDelayLoadedDLL2(Dll))
      return 4;
    // The table holds the thunks again, so the next call loads the DLL afresh.
    if (Unloaded)
      fprintf(stderr, "twice(4) = %d, loaded %d\n", twice(4), GetModuleHandleA(Dll) != NULL);
  }
  return 0;
}
#endif
