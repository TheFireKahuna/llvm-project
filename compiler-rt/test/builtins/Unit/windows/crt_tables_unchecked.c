// RUN: %clang_wincrt --target=x86_64-pc-windows-msvc -c -DFOREIGN %s -o %t-foreign.o
// RUN: %clang_wincrt %s %t-foreign.o -o %t.exe
// RUN: %run %t.exe 2>&1 | FileCheck %s
// RUN: llvm-readobj --coff-imports %t.exe | FileCheck %s --check-prefix=IMPORTS

// The start-up code calls the entries of the .CRT tables itself, skipping the
// null entries the linker may pad them with, and checks no KCFI type on the
// calls: an entry may come from foreign code, here an object built for the
// MSVC environment, whose functions carry no KCFI prefix. It does not import
// the UCRT's _initterm or _initterm_e.

typedef int (*InitializerFn)(void);
typedef void (*ConstructorFn)(void);

#pragma section(".CRT$XIU", read)
#pragma section(".CRT$XCU", read)
#pragma section(".CRT$XCV", read)
#pragma section(".CRT$XTU", read)

void report(const char *Message);

#ifdef FOREIGN
static int foreignInitializer(void) {
  report("foreign initializer");
  return 0;
}

static void foreignConstructor(void) { report("foreign constructor"); }

static void foreignTerminator(void) { report("foreign terminator"); }

__attribute__((used)) __declspec(allocate(".CRT$XIU")) static const
    InitializerFn Initializer = foreignInitializer;
__attribute__((used)) __declspec(allocate(".CRT$XCU")) static const
    ConstructorFn Constructor = foreignConstructor;
__attribute__((used)) __declspec(allocate(".CRT$XTU")) static const
    ConstructorFn Terminator = foreignTerminator;
#else
#include <stdio.h>

void report(const char *Message) { fprintf(stderr, "%s\n", Message); }

__attribute__((used)) __declspec(allocate(".CRT$XCV")) static const
    ConstructorFn Null = 0;

int main(void) {
  report("main");
  return 0;
}
#endif

// CHECK:      foreign initializer
// CHECK-NEXT: foreign constructor
// CHECK-NEXT: main
// CHECK-NEXT: foreign terminator

// IMPORTS-NOT: _initterm
