// RUN: %clang_wincrt -mguard=cf %s -o %t.exe
// RUN: %run %t.exe
// RUN: not --crash %run %t.exe invalid-target
// RUN: %clang_wincrt -mguard=cf -c %s -o %t.o
// RUN: %clang_wincrt %t.o -o %t-unguarded.exe
// RUN: %run %t-unguarded.exe
// RUN: %run %t-unguarded.exe invalid-target

// The load configuration names wincrt's Control Flow Guard pointers, which
// stay read-only. In a guarded image the loader installs its validators, an
// indirect call to an invalid target fails fast, and the unchecked dispatch
// stub is not a valid target. Instrumented code linked into an unguarded
// image calls through wincrt's stubs, which check nothing.

#include <string.h>
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;
extern void *volatile __guard_check_icall_fptr;
extern void *volatile __guard_dispatch_icall_fptr;
extern char __wincrt_guard_dispatch_icall_nop[];
int _guard_icall_checks_enforced(void);

__attribute__((noinline)) static int add(int A, int B) { return A + B; }

static const IMAGE_NT_HEADERS64 *ntHeaders(void) {
  const char *Base = (const char *)&__ImageBase;
  return (const IMAGE_NT_HEADERS64 *)(Base + __ImageBase.e_lfanew);
}

static const IMAGE_LOAD_CONFIG_DIRECTORY64 *loadConfig(void) {
  DWORD Rva =
      ntHeaders()
          ->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG]
          .VirtualAddress;
  return (const IMAGE_LOAD_CONFIG_DIRECTORY64 *)((const char *)&__ImageBase +
                                                 Rva);
}

static int isReadOnly(const void *Address) {
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Address, &Info, sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}

// Whether the guard function table lists the 16-byte granule of Address,
// which would make it a valid call target.
static int isValidTarget(const IMAGE_LOAD_CONFIG_DIRECTORY64 *Config,
                         const void *Address) {
  ULONG_PTR Rva = (ULONG_PTR)Address - (ULONG_PTR)&__ImageBase;
  const unsigned char *Table =
      (const unsigned char *)Config->GuardCFFunctionTable;
  size_t Stride = sizeof(DWORD) + ((Config->GuardFlags &
                                    IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_MASK) >>
                                   IMAGE_GUARD_CF_FUNCTION_TABLE_SIZE_SHIFT);
  for (ULONGLONG I = 0; I != Config->GuardCFFunctionCount; ++I) {
    DWORD Entry;
    memcpy(&Entry, Table + I * Stride, sizeof(Entry));
    if ((Entry & ~15u) == (Rva & ~(ULONG_PTR)15))
      return 1;
  }
  return 0;
}

// Calls code that returns at once, from memory whose call targets are all
// invalid when the process enforces the guard.
static void callInvalidTarget(int Guarded) {
#if defined(__x86_64__)
  static const unsigned char Return[] = {0xc3};
#else
  static const unsigned char Return[] = {0xc0, 0x03, 0x5f, 0xd6};
#endif
  DWORD Protect = PAGE_EXECUTE_READWRITE;
  if (Guarded)
    Protect |= PAGE_TARGETS_INVALID;
  void *Code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, Protect);
  if (!Code)
    ExitProcess(20);
  memcpy(Code, Return, sizeof(Return));
  DWORD Old;
  Protect = PAGE_EXECUTE_READ;
  if (Guarded)
    Protect |= PAGE_TARGETS_NO_UPDATE;
  if (!VirtualProtect(Code, 4096, Protect, &Old) ||
      !FlushInstructionCache(GetCurrentProcess(), Code, sizeof(Return)))
    ExitProcess(21);
  void (*volatile Target)(void) = (void (*)(void))Code;
  Target();
}

int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  const IMAGE_LOAD_CONFIG_DIRECTORY64 *Config = loadConfig();
  if (Config->GuardCFCheckFunctionPointer !=
          (ULONGLONG)&__guard_check_icall_fptr ||
      Config->GuardCFDispatchFunctionPointer !=
          (ULONGLONG)&__guard_dispatch_icall_fptr)
    return 1;
  if (!isReadOnly((const void *)&__guard_check_icall_fptr) ||
      !isReadOnly((const void *)&__guard_dispatch_icall_fptr))
    return 2;

  int Guarded = (ntHeaders()->OptionalHeader.DllCharacteristics &
                 IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;
  if (_guard_icall_checks_enforced() != Guarded)
    return 3;
  if (Guarded) {
    if (isValidTarget(Config, __wincrt_guard_dispatch_icall_nop))
      return 4;
  } else if (__guard_dispatch_icall_fptr !=
             (void *)__wincrt_guard_dispatch_icall_nop) {
    return 5;
  }

  int (*volatile Add)(int, int) = add;
  if (Add(2, 3) != 5)
    return 6;

  if (argc > 1 && strcmp(argv[1], "invalid-target") == 0)
    callInvalidTarget(Guarded);
  return 0;
}
