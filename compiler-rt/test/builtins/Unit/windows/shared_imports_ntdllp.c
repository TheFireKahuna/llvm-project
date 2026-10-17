// REQUIRES: ntdllp
// RUN: %clang_wincrt %s -lntdllp -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe > %t.imports
// RUN: FileCheck %s --check-prefix=NTDLL < %t.imports
// RUN: FileCheck %s --check-prefix=UCRTBASE < %t.imports
// RUN: FileCheck %s --check-prefix=STRING < %t.imports
// RUN: %clang_wincrt -static %s -lntdllp -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t-static.exe > %t-static.imports
// RUN: FileCheck %s --check-prefix=NTDLL < %t-static.imports
// RUN: FileCheck %s --check-prefix=UCRTBASE < %t-static.imports
// RUN: FileCheck %s --check-prefix=STRING < %t-static.imports

// The Windows SDK's private ntdllp.lib imports memset, strlen,
// __C_specific_handler and other Universal CRT functions from ntdll.dll. A
// program that links it still binds the Universal CRT's, under their own
// names. ntdll's __C_specific_handler differs: when an __except accepts a
// Visual C++ exception, the Universal CRT's destroys the exception object
// before unwinding to the handler, and ntdll's does not.

#include <stdio.h>
#include <string.h>
#include <windows.h>

// The parameters of a Visual C++ throw on a 64-bit target: the magic number,
// the object, its throw information and the image base the throw
// information's offsets are relative to.
#define CXX_EXCEPTION 0xE06D7363
#define CXX_MAGIC 0x19930520

struct ThrowInfo {
  unsigned Attributes;
  int Destructor;
  int ForwardCompat;
  int CatchableTypes;
};

extern IMAGE_DOS_HEADER __ImageBase;

static int Destroyed;

static void destroy(void *Object) { Destroyed = *(int *)Object; }

static void throwObject(void) {
  static int Object = 42;
  static struct ThrowInfo Info;
  Info.Destructor = (int)((char *)destroy - (char *)&__ImageBase);
  ULONG_PTR Parameters[] = {CXX_MAGIC, (ULONG_PTR)&Object, (ULONG_PTR)&Info,
                            (ULONG_PTR)&__ImageBase};
  RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 4, Parameters);
}

int main(int argc, char **argv) {
  char Buffer[16];
  memset(Buffer, 'x', sizeof(Buffer) - 1);
  Buffer[sizeof(Buffer) - 1] = 0;
  printf("strlen %zu\n", strlen(Buffer));

  HMODULE Ucrt = GetModuleHandleW(L"ucrtbase.dll");
  printf("memset %d, strlen %d\n",
         (void *)GetProcAddress(Ucrt, "memset") == (void *)memset,
         (void *)GetProcAddress(Ucrt, "strlen") == (void *)strlen);

  __try {
    throwObject();
  } __except (GetExceptionCode() == CXX_EXCEPTION ? EXCEPTION_EXECUTE_HANDLER
                                                  : EXCEPTION_CONTINUE_SEARCH) {
    printf("caught, destroyed %d\n", Destroyed);
  }
  return 0;
}

// CHECK: strlen 15
// CHECK-NEXT: memset 1, strlen 1
// CHECK-NEXT: caught, destroyed 42

// NTDLL:     Name: ntdll.dll
// NTDLL-NOT: Symbol: {{memset|strlen|__C_specific_handler}} (
// NTDLL:     }

// UCRTBASE:     Name: ucrtbase.dll
// UCRTBASE-DAG: Symbol: __C_specific_handler (0)
// UCRTBASE-DAG: Symbol: memset (0)
// UCRTBASE:     }

// STRING:      Name: api-ms-win-crt-string-l1-1-0.dll
// STRING-NEXT: ImportLookupTableRVA:
// STRING-NEXT: ImportAddressTableRVA:
// STRING-NEXT: Symbol: strlen (0)
