// /delayload imports resolve on first call through __delayLoadHelper2, both
// on the loader's native path and on the manual path taken when a delayimp.h
// notify hook is installed. The address table they resolve into is a section
// of its own that the loader keeps read-only.
// RUN: %clangxx_crt_dll -O2 -DBUILD_DLL %s -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -O2 %s %t.lib -o %t.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t.exe %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=NATIVE
// RUN: %clangxx_crt_main -O2 -DUSE_HOOK %s %t.lib -o %t.hook.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t.hook.exe %basename_t.tmp.dll 2>&1 | FileCheck %s --check-prefix=HOOK
// REQUIRES: windows, crt

#include <stdio.h>

#if defined(BUILD_DLL)
extern "C" __declspec(dllexport) int delayed(int value) { return value * 2; }
#else
#  define WIN32_LEAN_AND_MEAN
#  include <assert.h>
#  include <windows.h>

extern "C" __declspec(dllimport) int delayed(int);
extern "C" HRESULT __stdcall __HrLoadAllImportsForDll(LPCSTR);
extern "C" IMAGE_DOS_HEADER __ImageBase;

// The protection the loader applies covers the whole section holding the first
// descriptor's address table, so querying that table reports it.
static DWORD delayIatProtection() {
  auto *Base = reinterpret_cast<const char *>(&__ImageBase);
  auto *Nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(Base +
                                                          __ImageBase.e_lfanew);
  DWORD Rva = Nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]
                  .VirtualAddress;
  assert(Rva);
  auto *Descriptor =
      reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR *>(Base + Rva);
  MEMORY_BASIC_INFORMATION Info;
  assert(VirtualQuery(Base + Descriptor->ImportAddressTableRVA, &Info,
                      sizeof(Info)));
  return Info.Protect;
}

#  if defined(USE_HOOK)
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
static const char *stages[] = {"start", "pre-load", "pre-getproc",
                               "fail-load", "fail-getproc", "end"};
static void *__stdcall hook(unsigned reason, DelayLoadInfo *info) {
  fprintf(stderr, "hook %s %s", stages[reason], info->szDll);
  if (reason == 2 || reason == 5)
    fprintf(stderr, " %s", info->dlp.szProcName);
  fprintf(stderr, "\n");
  return nullptr;
}
extern "C" const void *__pfnDliNotifyHook2 = (const void *)hook;
#  endif

int main(int argc, char **argv) {
  assert(argc == 2);
  assert(!GetModuleHandleA(argv[1]));
  fprintf(stderr, "before call %lx\n", delayIatProtection());
  int result = delayed(21);
  fprintf(stderr, "delayed(21) = %d\n", result);
  assert(GetModuleHandleA(argv[1]));
  assert(delayed(4) == 8);
  assert(__HrLoadAllImportsForDll(argv[1]) == S_OK);
  assert(__HrLoadAllImportsForDll("no-such-module.dll") != S_OK);
  // Resolving restores the protection rather than leaving the table open.
  fprintf(stderr, "done %lx\n", delayIatProtection());
  return 0;
}
#endif

// PAGE_READONLY both before the first call and after the table is written.
// NATIVE: before call 2
// NATIVE-NEXT: delayed(21) = 42
// NATIVE-NEXT: done 2

// HOOK: before call 2
// HOOK-NEXT: hook start {{.*}}.dll
// HOOK-NEXT: hook pre-load {{.*}}.dll
// HOOK-NEXT: hook pre-getproc {{.*}}.dll delayed
// HOOK-NEXT: hook end {{.*}}.dll delayed
// HOOK-NEXT: delayed(21) = 42
// HOOK-NEXT: done 2
