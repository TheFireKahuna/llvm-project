// RUN: %clang_wincrt -static %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORT \
// RUN:     --implicit-check-not=clang_rt.wincrt_dynamic.dll
// RUN: llvm-readobj --coff-tls-directory %t.exe | FileCheck %s --check-prefix=TLS

// With -static, the termination registries are in the executable, which
// imports no DLL for them, and they run its registrations as the DLL does.
// A program that registers no thread-local destructor links no thread
// registry, so it has no TLS directory and no TLS callback.

#include <stdio.h>
#include <stdlib.h>

static void first(void) { printf("atexit 1\n"); }
static void second(void) { printf("atexit 2\n"); }

int main(void) {
  atexit(first);
  atexit(second);
  return 0;
}

// CHECK:      atexit 2
// CHECK-NEXT: atexit 1

// IMPORT: Name: ucrtbase.dll

// TLS:      TLSDirectory {
// TLS-NEXT: }
