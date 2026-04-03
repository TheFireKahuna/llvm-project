// Test .CRT$X* section ordering (alphabetical execution order).
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

typedef void (*_PVFV)(void);
typedef int (*_PIFV)(void);

// C initializers (.CRT$XI*) run before C++ constructors (.CRT$XC*).
#pragma section(".CRT$XIB", long, read)
static int c_init_early() {
  printf("C init early (.CRT$XIB)\n");
  return 0;
}
__declspec(allocate(".CRT$XIB")) static _PIFV c_init_early_ptr = c_init_early;

#pragma section(".CRT$XIY", long, read)
static int c_init_late() {
  printf("C init late (.CRT$XIY)\n");
  return 0;
}
__declspec(allocate(".CRT$XIY")) static _PIFV c_init_late_ptr = c_init_late;

#pragma section(".CRT$XCB", long, read)
static void cpp_ctor_early() {
  printf("C++ ctor early (.CRT$XCB)\n");
}
__declspec(allocate(".CRT$XCB")) static _PVFV cpp_ctor_early_ptr = cpp_ctor_early;

#pragma section(".CRT$XCY", long, read)
static void cpp_ctor_late() {
  printf("C++ ctor late (.CRT$XCY)\n");
}
__declspec(allocate(".CRT$XCY")) static _PVFV cpp_ctor_late_ptr = cpp_ctor_late;

struct GlobalObject {
  GlobalObject() {
    printf("GlobalObject ctor (.CRT$XCU)\n");
  }
  ~GlobalObject() {
    printf("GlobalObject dtor\n");
  }
};
GlobalObject g_obj;

int main() {
  printf("main() running\n");
  return 0;
}

// CHECK: C init early (.CRT$XIB)
// CHECK: C init late (.CRT$XIY)
// CHECK: C++ ctor early (.CRT$XCB)
// CHECK: GlobalObject ctor (.CRT$XCU)
// CHECK: C++ ctor late (.CRT$XCY)
// CHECK: main() running
// CHECK: GlobalObject dtor
