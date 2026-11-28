// RUN: %clang -Xclang -cfguard %s -Wl,-debug:symtab -o %t.exe
// RUN: %run %t.exe
// RUN: %run %t.exe invalid-target
// RUN: %clang -Xclang -cfguard %s -Wl,-guard:cf,-debug:symtab -o %t-guarded.exe
// RUN: %run %t-guarded.exe
// RUN: not --crash %run %t-guarded.exe invalid-target
// RUN: llvm-nm %t-guarded.exe > %t.txt
// RUN: llvm-readobj --coff-load-config %t-guarded.exe >> %t.txt
// RUN: FileCheck %s --input-file=%t.txt

// The load configuration names wincrt's Control Flow Guard pointers, which
// are read-only. In an image the loader does not guard they hold wincrt's
// stubs, which check nothing, so instrumented code runs as it would without
// the guard. In a guarded image the loader installs its validators, and an
// indirect call to an invalid target fails fast. Neither stub is itself a
// valid target.

// CHECK:      [[#%x,NOP:]] t __wincrt_guard_check_nop
// CHECK:      [[#%x,DISPATCH:]] t __wincrt_guard_dispatch_nop
// CHECK:      GuardCFCheckFunction: 0x14{{.+}}
// CHECK-NEXT: GuardCFCheckDispatch: 0x14{{.+}}
// CHECK:      GuardFidTable [
// CHECK-NOT:  0x[[#%X,NOP]]
// CHECK-NOT:  0x[[#%X,DISPATCH]]
// CHECK:      ]

#include <string.h>
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;
extern void *volatile __guard_check_icall_fptr;
extern void *volatile __guard_dispatch_icall_fptr;
int _guard_icall_checks_enforced(void);

__attribute__((noinline)) static int add(int A, int B) { return A + B; }

static int isReadOnly(const void *Address) {
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Address, &Info, sizeof(Info)) &&
         Info.Protect == PAGE_READONLY;
}

// Calls code that returns at once, in memory none of whose addresses is a
// valid call target where the process enforces the guard.
static void callInvalidTarget(int Guarded) {
#if defined(__x86_64__)
  static const unsigned char Return[] = {0xc3};
#else
  static const unsigned char Return[] = {0xc0, 0x03, 0x5f, 0xd6};
#endif
  void *Code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                            PAGE_EXECUTE_READWRITE |
                                (Guarded ? PAGE_TARGETS_INVALID : 0));
  if (!Code)
    ExitProcess(20);
  memcpy(Code, Return, sizeof(Return));
  FlushInstructionCache(GetCurrentProcess(), Code, sizeof(Return));
  void (*volatile Target)(void) = (void (*)(void))Code;
  Target();
}

int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (!isReadOnly((const void *)&__guard_check_icall_fptr) ||
      !isReadOnly((const void *)&__guard_dispatch_icall_fptr))
    return 1;

  const IMAGE_NT_HEADERS *Headers =
      (const IMAGE_NT_HEADERS *)((const char *)&__ImageBase +
                                 __ImageBase.e_lfanew);
  int Guarded = (Headers->OptionalHeader.DllCharacteristics &
                 IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;
  if (_guard_icall_checks_enforced() != Guarded)
    return 2;

  int (*volatile Add)(int, int) = add;
  if (Add(2, 3) != 5)
    return 3;

  if (argc > 1)
    callInvalidTarget(Guarded);
  return 0;
}
