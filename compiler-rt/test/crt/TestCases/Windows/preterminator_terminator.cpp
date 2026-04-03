// Test .CRT$XPA (pre-terminator) and .CRT$XTA (terminator) sections.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

typedef void (*_PVFV)(void);

static int g_order = 0;

// Pre-terminators run after atexit handlers.
#pragma section(".CRT$XPB", long, read)
static void preterminator_early() {
  printf("preterminator_early (.CRT$XPB) at order %d\n", g_order++);
}
__declspec(allocate(".CRT$XPB")) static _PVFV pre_early_ptr = preterminator_early;

#pragma section(".CRT$XPY", long, read)
static void preterminator_late() {
  printf("preterminator_late (.CRT$XPY) at order %d\n", g_order++);
}
__declspec(allocate(".CRT$XPY")) static _PVFV pre_late_ptr = preterminator_late;

// Terminators run after pre-terminators.
#pragma section(".CRT$XTB", long, read)
static void terminator_early() {
  printf("terminator_early (.CRT$XTB) at order %d\n", g_order++);
}
__declspec(allocate(".CRT$XTB")) static _PVFV term_early_ptr = terminator_early;

#pragma section(".CRT$XTY", long, read)
static void terminator_late() {
  printf("terminator_late (.CRT$XTY) at order %d\n", g_order++);
}
__declspec(allocate(".CRT$XTY")) static _PVFV term_late_ptr = terminator_late;

struct Destructor {
  ~Destructor() {
    printf("Destructor at order %d\n", g_order++);
  }
};

static Destructor g_dtor;

int main() {
  // CHECK: Pre-terminator/terminator test
  printf("Pre-terminator/terminator test\n");

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Exit ordering:
// 1. Destructors (via __cxa_finalize)
// 2. Pre-terminators (alphabetical by section name: XPB < XPY)
// 3. Terminators (alphabetical: XTB < XTY)

// CHECK: Destructor at order 0
// CHECK: preterminator_early (.CRT$XPB) at order 1
// CHECK: preterminator_late (.CRT$XPY) at order 2
// CHECK: terminator_early (.CRT$XTB) at order 3
// CHECK: terminator_late (.CRT$XTY) at order 4
