// RUN: %clang_wincrt -shared -DBUILD_DLL %s -o %t.dll
// RUN: %clang_wincrt %s %t.dll.lib -o %t.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t.exe %basename_t.tmp.dll 2>&1 | FileCheck %s
// RUN: %clang_wincrt -mguard=cf -DPROTECTED %s %t.dll.lib -o %t-protected.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t-protected.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefixes=CHECK,PROTECTED
// RUN: %clang_wincrt -DNOTIFY_HOOK %s %t.dll.lib -o %t-hook.exe \
// RUN:   -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t-hook.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefixes=CHECK,HOOK
// RUN: %clang_wincrt -mguard=cf -DPROTECTED -DNOTIFY_HOOK %s %t.dll.lib \
// RUN:   -o %t-protected-hook.exe -Wl,/delayload:%basename_t.tmp.dll
// RUN: %run %t-protected-hook.exe %basename_t.tmp.dll 2>&1 \
// RUN:   | FileCheck %s --check-prefixes=CHECK,PROTECTED,HOOK

// A delayed import resolves on its first call, through the loader or, with a
// delayimp.h notify hook installed, in the image, where the hook sees every
// stage of each import as it is called. Under /guard:cf the table is
// protected: read-only before and after it is written.

#include <stdio.h>
#include <windows.h>

#ifdef BUILD_DLL
__declspec(dllexport) int twice(int Value) { return 2 * Value; }
__declspec(dllexport) int add(int A, int B) { return A + B; }
#else

__declspec(dllimport) int twice(int);
__declspec(dllimport) int add(int, int);
HRESULT WINAPI __HrLoadAllImportsForDll(LPCSTR);
extern IMAGE_DOS_HEADER __ImageBase;

#  ifdef PROTECTED
// Whether the page of the first descriptor's address table is read-only.
static int tableIsReadOnly(void) {
  const char *Base = (const char *)&__ImageBase;
  const IMAGE_NT_HEADERS64 *Nt =
      (const IMAGE_NT_HEADERS64 *)(Base + __ImageBase.e_lfanew);
  const IMAGE_DATA_DIRECTORY *Directory =
      &Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  const IMAGE_DELAYLOAD_DESCRIPTOR *Descriptor =
      (const IMAGE_DELAYLOAD_DESCRIPTOR *)(Base + Directory->VirtualAddress);
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Base + Descriptor->ImportAddressTableRVA, &Info,
                      sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}
#  endif

#  ifdef NOTIFY_HOOK
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

static FARPROC WINAPI notifyHook(unsigned Reason, DelayLoadInfo *Info) {
  static const char *const Stages[] = {
      "start", "pre-load", "pre-getproc", "fail-load", "fail-getproc", "end"};
  fprintf(stderr, "hook %s %s\n", Stages[Reason], Info->dlp.szProcName);
  return NULL;
}
FARPROC(WINAPI *const __pfnDliNotifyHook2)(unsigned,
                                           DelayLoadInfo *) = notifyHook;
#  endif

int main(int argc, char **argv) {
  if (argc != 2 || GetModuleHandleA(argv[1]))
    return 1;
#  ifdef PROTECTED
  fprintf(stderr, "read-only before %d\n", tableIsReadOnly());
#  endif
  fprintf(stderr, "twice(21) = %d\n", twice(21));
  if (!GetModuleHandleA(argv[1]))
    return 2;
  fprintf(stderr, "add(1, 2) = %d\n", add(1, 2));
  fprintf(stderr, "load all %ld\n", (long)__HrLoadAllImportsForDll(argv[1]));
  fprintf(stderr, "load all of another %s\n",
          __HrLoadAllImportsForDll("no-such-library.dll") ==
                  HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)
              ? "not found"
              : "found");
#  ifdef PROTECTED
  fprintf(stderr, "read-only after %d\n", tableIsReadOnly());
#  endif
  return 0;
}
#endif

// PROTECTED:      read-only before 1
// HOOK:           hook start twice
// HOOK-NEXT:      hook pre-load twice
// HOOK-NEXT:      hook pre-getproc twice
// HOOK-NEXT:      hook end twice
// CHECK:          twice(21) = 42
// HOOK-NEXT:      hook start add
// HOOK-NEXT:      hook pre-getproc add
// HOOK-NEXT:      hook end add
// CHECK:          add(1, 2) = 3
// CHECK-NEXT:     load all 0
// CHECK-NEXT:     load all of another not found
// PROTECTED-NEXT: read-only after 1
