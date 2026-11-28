// RUN: %clang -c %s -o %t.o
// RUN: %clang %t.o -Wl,-guard:no -Wl,-debug:symtab -o %t.exe
// RUN: %run %t.exe
// RUN: %run %t.exe invalid-target
// RUN: llvm-nm %t.exe > %t.txt
// RUN: llvm-readobj --coff-load-config %t.exe >> %t.txt
// RUN: FileCheck %s --input-file=%t.txt --check-prefixes=CHECK,UNGUARDED
// RUN: %clang %t.o -Wl,-debug:symtab -o %t-guarded.exe
// RUN: %run %t-guarded.exe
// RUN: not --crash %run %t-guarded.exe invalid-target
// RUN: llvm-nm %t-guarded.exe > %t-guarded.txt
// RUN: llvm-readobj --coff-load-config %t-guarded.exe >> %t-guarded.txt
// RUN: FileCheck %s --input-file=%t-guarded.txt --check-prefixes=CHECK,GUARDED

// The load configuration names the security cookie, a read-only word that the
// loader replaces with a 48-bit value before main, and protected frames check
// against it. It names the Control Flow Guard pointers, also read-only. In an
// image the loader does not guard they hold the builtins' stubs, which check
// nothing, so instrumented code runs as it would without the guard, and the
// fields of the absent features are zero. In a guarded image the loader
// installs its validators, and an indirect call to an invalid target fails
// fast. Neither stub is itself a valid target.

// CHECK-DAG:      [[#%x,COOKIE:]] R __security_cookie
// CHECK-DAG:      [[#%x,NOP:]] t __guard_check_icall_nop
// CHECK-DAG:      [[#%x,DISPATCH:]] t __guard_dispatch_icall_nop
// CHECK:          LoadConfig [
// CHECK-NEXT:       Size: 0x148
// CHECK:            SecurityCookie: 0x[[#%X,COOKIE]]
// CHECK:            GuardCFCheckFunction: 0x14{{.+}}
// CHECK-NEXT:       GuardCFCheckDispatch: 0x14{{.+}}
// UNGUARDED-NEXT:   GuardCFFunctionTable: 0x0
// UNGUARDED-NEXT:   GuardCFFunctionCount: 0
// UNGUARDED:        GuardAddressTakenIatEntryTable: 0x0
// UNGUARDED:        EnclaveConfigurationPointer: 0x0
// UNGUARDED-NEXT:   VolatileMetadataPointer: 0x0
// UNGUARDED-NEXT:   GuardEHContinuationTable: 0x0
// UNGUARDED-NEXT:   GuardEHContinuationCount: 0
// GUARDED:        GuardFidTable [
// GUARDED-NOT:      0x[[#%X,NOP]]
// GUARDED-NOT:      0x[[#%X,DISPATCH]]
// GUARDED:        ]

#include <stdint.h>
#include <string.h>
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;
extern const uintptr_t __security_cookie;
extern void *volatile __guard_check_icall_fptr;
extern void *volatile __guard_dispatch_icall_fptr;
int _guard_icall_checks_enforced(void);

__attribute__((noinline)) static int add(int A, int B) { return A + B; }

__attribute__((noinline)) static int protectedFrame(const char *Text) {
  char Buffer[64];
  strcpy(Buffer, Text);
  return (int)strlen(Buffer);
}

static int isReadOnly(const void *Address) {
  MEMORY_BASIC_INFORMATION Info;
  return VirtualQuery(Address, &Info, sizeof(Info)) && Info.Protect == PAGE_READONLY;
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
                            PAGE_EXECUTE_READWRITE | (Guarded ? PAGE_TARGETS_INVALID : 0));
  if (!Code)
    ExitProcess(20);
  memcpy(Code, Return, sizeof(Return));
  FlushInstructionCache(GetCurrentProcess(), Code, sizeof(Return));
  void (*volatile Target)(void) = (void (*)(void))Code;
  Target();
}

int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (__security_cookie == 0x2B992DDFA232 || __security_cookie >> 48 ||
      protectedFrame("protected") != 9)
    return 1;
  if (!isReadOnly(&__security_cookie) ||
      !isReadOnly((const void *)&__guard_check_icall_fptr) ||
      !isReadOnly((const void *)&__guard_dispatch_icall_fptr))
    return 2;
  const IMAGE_NT_HEADERS *Headers =
      (const IMAGE_NT_HEADERS *)((const char *)&__ImageBase + __ImageBase.e_lfanew);
  int Guarded =
      (Headers->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;
  int (*volatile Add)(int, int) = add;
  if (_guard_icall_checks_enforced() != Guarded || Add(2, 3) != 5)
    return 3;
  if (argc > 1)
    callInvalidTarget(Guarded);
  return 0;
}
