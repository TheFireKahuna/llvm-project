// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe 2>&1 | FileCheck %s
// RUN: %clang_wincrt -DFAIL %s -o %t-fail.exe
// RUN: not %run %t-fail.exe 2>&1 | FileCheck %s --check-prefix=FAIL

// The C initializers in .CRT$XI run in section order, then the constructors
// in .CRT$XC, then main. A C initializer that fails ends start-up with an
// error, and nothing after it runs.

#include <stdio.h>

typedef int (*InitializerFn)(void);
typedef void (*ConstructorFn)(void);

static int first(void) {
  fprintf(stderr, "first initializer\n");
  return 0;
}

static int second(void) {
  fprintf(stderr, "second initializer\n");
#ifdef FAIL
  return 1;
#else
  return 0;
#endif
}

static void constructor(void) { fprintf(stderr, "constructor\n"); }

#pragma section(".CRT$XIU", read)
#pragma section(".CRT$XIV", read)
#pragma section(".CRT$XCU", read)
__attribute__((used))
__declspec(allocate(".CRT$XIV")) static const InitializerFn Second = second;
__attribute__((used))
__declspec(allocate(".CRT$XIU")) static const InitializerFn First = first;
__attribute__((used))
__declspec(allocate(".CRT$XCU")) static const ConstructorFn Constructor =
    constructor;

int main(void) {
  fprintf(stderr, "main\n");
  return 0;
}

// CHECK:      first initializer
// CHECK-NEXT: second initializer
// CHECK-NEXT: constructor
// CHECK-NEXT: main

// FAIL:      first initializer
// FAIL-NEXT: second initializer
// FAIL-NEXT: wincrt: a C initializer failed
// FAIL-NOT:  {{.}}
