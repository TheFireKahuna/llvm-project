// RUN: %clang_crt_main_cfg %s -O2 -o %t.exe
// RUN: %clang_crt_main_cfg %s -O2 -DBUILD_DLL -shared -o %t.dll
// RUN: %run %t.exe
// RUN: %clang_crt_main_cfg %s -O2 -DNO_CFG -Wl,/guard:no -o %t-fallback.exe
// RUN: %clang_crt_main_cfg %s -O2 -DNO_CFG -DBUILD_DLL -shared -Wl,/guard:no -o %t-fallback.dll
// RUN: %run %t-fallback.exe
// RUN: %clang_crt_main_cfg %s -O2 -lc++ -o %t-shared.exe
// RUN: %clang_crt_main_cfg %s -O2 -lc++ -DBUILD_DLL -shared -o %t-shared.dll
// RUN: %run %t-shared.exe
// REQUIRES: windows, crt, x86_64

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

extern int _guard_icall_checks_enforced(void);
extern char __wincrt_guard_dispatch_icall_nop[];
extern char __guard_flags[];
// An absolute symbol is read through a statically initialised pointer that
// the compiler cannot fold: code cannot address it relative to the
// instruction pointer.
static const char *volatile guard_flags = __guard_flags;
extern int __cxa_atexit(void (*)(void *), void *, void *);

static void check_readonly(uintptr_t address) {
  MEMORY_BASIC_INFORMATION info;
  assert(VirtualQuery((void *)address, &info, sizeof(info)) == sizeof(info));
  assert(info.State == MEM_COMMIT);
  assert(info.Protect == PAGE_READONLY);
}

__attribute__((noinline)) static double
mixed_args(unsigned a, double b, unsigned c, double d, unsigned e, double f) {
  return a + b + c + d + e + f;
}

static void check_metadata(void) {
  // The EXE holds the load reference while the DLL's checks execute.
  HMODULE module;
  assert(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&check_metadata, &module));
  char *base = (char *)module;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  IMAGE_DATA_DIRECTORY directory =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
  IMAGE_LOAD_CONFIG_DIRECTORY *lc =
      (IMAGE_LOAD_CONFIG_DIRECTORY *)(base + directory.VirtualAddress);
  assert(lc->Size == directory.Size);
  assert(lc->Size >= offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, CodeIntegrity) +
                         sizeof(lc->CodeIntegrity));
  assert(lc->CodeIntegrity.Flags == 0 && lc->CodeIntegrity.Catalog == 0);
  assert(lc->CodeIntegrity.CatalogOffset == 0 &&
         lc->CodeIntegrity.Reserved == 0);
  assert(lc->GuardFlags == (DWORD)(uintptr_t)guard_flags);
  check_readonly((uintptr_t)lc);
  check_readonly(lc->GuardCFCheckFunctionPointer);
  check_readonly(lc->GuardCFDispatchFunctionPointer);

#ifdef NO_CFG
  assert(!(nt->OptionalHeader.DllCharacteristics &
           IMAGE_DLLCHARACTERISTICS_GUARD_CF));
  assert(lc->GuardFlags == IMAGE_GUARD_CF_INSTRUMENTED);
  assert(lc->GuardCFFunctionTable == 0 && lc->GuardCFFunctionCount == 0);
  assert(!_guard_icall_checks_enforced());
  assert(*(void **)lc->GuardCFDispatchFunctionPointer ==
         (void *)__wincrt_guard_dispatch_icall_nop);
#else
  assert(nt->OptionalHeader.DllCharacteristics &
         IMAGE_DLLCHARACTERISTICS_GUARD_CF);
  const DWORD required =
      IMAGE_GUARD_CF_INSTRUMENTED | IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT;
  assert((lc->GuardFlags & required) == required);
  assert(lc->GuardCFFunctionTable && lc->GuardCFFunctionCount);
  assert(_guard_icall_checks_enforced());
  assert(*(void **)lc->GuardCFDispatchFunctionPointer !=
         (void *)__wincrt_guard_dispatch_icall_nop);

  // The unchecked tail-call stub must not be a valid CFG target, including
  // through a different function sharing the same 16-byte CFG granule.
  uintptr_t stub_rva =
      (uintptr_t)__wincrt_guard_dispatch_icall_nop - (uintptr_t)base;
  const unsigned char *table = (const unsigned char *)lc->GuardCFFunctionTable;
  size_t stride = sizeof(DWORD) + (lc->GuardFlags >> 28);
  for (size_t i = 0; i != lc->GuardCFFunctionCount; ++i)
    assert((*(const DWORD *)(table + i * stride) & ~(DWORD)15) !=
           (stub_rva & ~(uintptr_t)15));
#endif

  double (*volatile call)(unsigned, double, unsigned, double, unsigned,
                          double) = mixed_args;
  assert(call(1, 2, 3, 4, 5, 6) == 21);
}

static unsigned dynamic_calls;
static void check_dynamic_call(void) { assert(dynamic_calls == 1); }

static int invalid_target(const char *mode) {
  if (strcmp(mode, "valid-dtor") == 0) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
    assert(code);
    // x64: incl (%rcx); ret. This callback is executable but belongs to no DLL.
    code[0] = 0xff;
    code[1] = 0x01;
    code[2] = 0xc3;
    DWORD old;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), code, 3));
    assert(atexit(check_dynamic_call) == 0);
    assert(__cxa_atexit((void (*)(void *))code, &dynamic_calls, NULL) == 0);
    return 0;
  }
  // Callable code with an explicitly invalid CFG bitmap entry. Without the
  // check it simply returns; an access violation is not a passing result.
  unsigned char *code =
      VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                   PAGE_EXECUTE_READWRITE | PAGE_TARGETS_INVALID);
  if (!code)
    return 101;
  code[0] = 0xc3; // x64 RET
  DWORD old;
  if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ | PAGE_TARGETS_NO_UPDATE,
                      &old) ||
      !FlushInstructionCache(GetCurrentProcess(), code, 1))
    return 102;
  void (*volatile target)(void *) = (void (*)(void *))code;
  if (strcmp(mode, "dtor") == 0) {
    if (__cxa_atexit(target, NULL, NULL) != 0)
      return 103;
    // Test the real wincrt destructor dispatcher at normal process exit.
  } else {
    target(NULL);
  }
  return 0;
}

static void expect_exit(const wchar_t *mode, DWORD expected) {
  wchar_t path[MAX_PATH], command[MAX_PATH + 32];
  DWORD size = GetModuleFileNameW(NULL, path, MAX_PATH);
  assert(size && size < MAX_PATH);
  assert(swprintf(command, MAX_PATH + 32, L"\"%ls\" %ls", path, mode) > 0);
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process;
  assert(CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process));
  assert(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD status;
  assert(GetExitCodeProcess(process.hProcess, &status));
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  assert(status == expected);
}

#ifdef BUILD_DLL
__declspec(dllexport) void check_dll(void) { check_metadata(); }
#else
int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc == 2)
    return invalid_target(argv[1]);
  check_metadata();
  wchar_t path[MAX_PATH];
  DWORD size = GetModuleFileNameW(NULL, path, MAX_PATH);
  assert(size >= 4 && size < MAX_PATH);
  wcscpy(path + size - 4, L".dll");
  HMODULE module = LoadLibraryExW(path, NULL,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  assert(module);
  void (*check)(void) = (void (*)(void))GetProcAddress(module, "check_dll");
  assert(check);
  check();
  assert(FreeLibrary(module));
#  ifndef NO_CFG
  expect_exit(L"call", 0xc0000409UL); // CFG fail-fast, not an access violation.
  expect_exit(L"dtor", 0xc0000409UL);
#  endif
  expect_exit(L"valid-dtor", 0);
  puts("PASS");
  return 0;
}
#endif
