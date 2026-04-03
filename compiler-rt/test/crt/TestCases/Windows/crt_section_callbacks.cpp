// Test all CRT section callback types (.CRT$XI*, .CRT$XC*, .CRT$XP*, .CRT$XT*).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

typedef int (*_PIFV)(void);
typedef void (*_PVFV)(void);

static int g_order = 0;

// C Initializers (.CRT$XI*) - return int, can fail.
#pragma section(".CRT$XIB", long, read)
static int c_init_b() {
  printf("[%d] C init B (.CRT$XIB)\n", g_order++);
  return 0;  // Success.
}
__declspec(allocate(".CRT$XIB")) static _PIFV c_init_b_ptr = c_init_b;

#pragma section(".CRT$XIM", long, read)
static int c_init_m() {
  printf("[%d] C init M (.CRT$XIM)\n", g_order++);
  return 0;
}
__declspec(allocate(".CRT$XIM")) static _PIFV c_init_m_ptr = c_init_m;

#pragma section(".CRT$XIY", long, read)
static int c_init_y() {
  printf("[%d] C init Y (.CRT$XIY)\n", g_order++);
  return 0;
}
__declspec(allocate(".CRT$XIY")) static _PIFV c_init_y_ptr = c_init_y;

// C++ Constructors (.CRT$XC*) - return void.
#pragma section(".CRT$XCB", long, read)
static void cpp_ctor_b() {
  printf("[%d] C++ ctor B (.CRT$XCB)\n", g_order++);
}
__declspec(allocate(".CRT$XCB")) static _PVFV cpp_ctor_b_ptr = cpp_ctor_b;

#pragma section(".CRT$XCM", long, read)
static void cpp_ctor_m() {
  printf("[%d] C++ ctor M (.CRT$XCM)\n", g_order++);
}
__declspec(allocate(".CRT$XCM")) static _PVFV cpp_ctor_m_ptr = cpp_ctor_m;

#pragma section(".CRT$XCY", long, read)
static void cpp_ctor_y() {
  printf("[%d] C++ ctor Y (.CRT$XCY)\n", g_order++);
}
__declspec(allocate(".CRT$XCY")) static _PVFV cpp_ctor_y_ptr = cpp_ctor_y;

// Global C++ object - goes in .CRT$XCU.
struct GlobalCtor {
  GlobalCtor() {
    printf("[%d] GlobalCtor (.CRT$XCU)\n", g_order++);
  }
  ~GlobalCtor() {
    printf("[%d] GlobalCtor dtor\n", g_order++);
  }
};
static GlobalCtor g_ctor;

// Pre-terminators (.CRT$XP*) - run after atexit/destructors.
#pragma section(".CRT$XPB", long, read)
static void preterminator_b() {
  printf("[%d] Pre-terminator B (.CRT$XPB)\n", g_order++);
}
__declspec(allocate(".CRT$XPB")) static _PVFV preterminator_b_ptr = preterminator_b;

#pragma section(".CRT$XPY", long, read)
static void preterminator_y() {
  printf("[%d] Pre-terminator Y (.CRT$XPY)\n", g_order++);
}
__declspec(allocate(".CRT$XPY")) static _PVFV preterminator_y_ptr = preterminator_y;

// Terminators (.CRT$XT*) - run last.
#pragma section(".CRT$XTB", long, read)
static void terminator_b() {
  printf("[%d] Terminator B (.CRT$XTB)\n", g_order++);
}
__declspec(allocate(".CRT$XTB")) static _PVFV terminator_b_ptr = terminator_b;

#pragma section(".CRT$XTY", long, read)
static void terminator_y() {
  printf("[%d] Terminator Y (.CRT$XTY)\n", g_order++);
}
__declspec(allocate(".CRT$XTY")) static _PVFV terminator_y_ptr = terminator_y;

int main() {
  printf("[%d] main\n", g_order++);
  return 0;
}

// Initialization order (before main):
// CHECK: [0] C init B (.CRT$XIB)
// CHECK: [1] C init M (.CRT$XIM)
// CHECK: [2] C init Y (.CRT$XIY)
// CHECK: [3] C++ ctor B (.CRT$XCB)
// CHECK: [4] GlobalCtor (.CRT$XCU)
// CHECK: [5] C++ ctor M (.CRT$XCM)
// CHECK: [6] C++ ctor Y (.CRT$XCY)
// CHECK: [7] main

// Termination order (after main):
// CHECK: [8] GlobalCtor dtor
// CHECK: [9] Pre-terminator B (.CRT$XPB)
// CHECK: [10] Pre-terminator Y (.CRT$XPY)
// CHECK: [11] Terminator B (.CRT$XTB)
// CHECK: [12] Terminator Y (.CRT$XTY)
