// RUN: %python %S/Inputs/wrapped_names.py %clang | FileCheck %s --check-prefix=NAMES
// RUN: %clang -shared -DDLL %s -lntdllp -o %t.dll
// RUN: %clang %s -lntdllp -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS
// RUN: llvm-readobj --coff-imports %t.dll | FileCheck %s --check-prefix=NTDLL
// RUN: %clang -static %s -lntdllp -o %t-static.exe
// RUN: %run %t-static.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t-static.exe | FileCheck %s \
// RUN:     --check-prefix=STATIC

// The Windows SDK's private ntdllp.lib imports memset, strlen,
// __C_specific_handler and reduced forms of sprintf and other formatted I/O
// from ntdll.dll. Every entry object wraps each name that it and the C
// library both offer, so a program that links it still binds the Universal
// CRT's functions and the runtime's formatted I/O, under their own names.
// [NAMES] Every name an entry object wraps has its __wrap_ name defined for an
// image that imports the runtime and for one linked with -static, so that no
// image binds the Universal CRT's own.
// ntdll's sprintf formats no floating point, and its __C_specific_handler
// does not destroy a Visual C++ exception that an __except accepts.

// NAMES: wrapped {{[0-9]+}}, ntdllp's {{[0-9]+}}: __C_specific_handler {{.*}} _setjmp {{.*}} memset {{.*}} sprintf {{.*}} strlen

#include <stdio.h>
#include <string.h>
#include <windows.h>

#ifdef DLL

__declspec(dllexport) size_t format(char *Buffer, double Value) {
  sprintf(Buffer, "%.1f", Value);
  return strlen(Buffer);
}

#else

extern IMAGE_DOS_HEADER __ImageBase;

static int Destroyed;

static void destroy(void *Object) { Destroyed = *(int *)Object; }

// A Visual C++ throw of an int with a destructor: the magic number, the
// object, its throw information, whose second word is the destructor's
// offset, and the image base the offset is from.
static void throwObject(void) {
  static int Object = 42;
  static int Info[4];
  Info[1] = (int)((char *)destroy - (char *)&__ImageBase);
  ULONG_PTR Parameters[] = {0x19930520, (ULONG_PTR)&Object, (ULONG_PTR)Info,
                            (ULONG_PTR)&__ImageBase};
  RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, Parameters);
}

int main(void) {
  char Buffer[64];
  sprintf(Buffer, "%.1f|%s", 2.5, "a");
  printf("sprintf %s\n", Buffer);
  double Value = 0;
  sscanf("3.75", "%lf", &Value);
  printf("sscanf %.2f\n", Value);

  // A length the compiler cannot see, or it stores the bytes itself.
  volatile size_t Length = 15;
  memset(Buffer, 'x', Length);
  Buffer[15] = 0;
  printf("strlen %zu\n", strlen(Buffer));

  __try {
    throwObject();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    printf("caught, destroyed %d\n", Destroyed);
  }
  return 0;
}

#endif

// CHECK:      sprintf 2.5|a
// CHECK-NEXT: sscanf 3.75
// CHECK-NEXT: strlen 15
// CHECK-NEXT: caught, destroyed 42

// IMPORTS-DAG: Name: clang_rt.wincrt_dynamic.dll
// IMPORTS-DAG: Symbol: sprintf (
// IMPORTS-DAG: Symbol: sscanf (
// IMPORTS-DAG: Name: ucrtbase.dll
// IMPORTS-DAG: Symbol: __C_specific_handler (
// IMPORTS-DAG: Symbol: memset (
// IMPORTS-DAG: Symbol: strlen (

// The C library's names that the DLL uses are none of ntdll.dll's, which its
// start-up imports from.
// NTDLL:     Name: ntdll.dll
// NTDLL-NOT: Symbol: {{.*printf|.*scanf|memset|strlen|__C_specific_handler}}
// NTDLL:     Import {

// STATIC:     Name: ntdll.dll
// STATIC-NOT: Symbol: {{.*printf|.*scanf|memset|strlen|__C_specific_handler}}
// STATIC:     }
