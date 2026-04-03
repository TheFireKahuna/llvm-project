// Test __dso_handle is correctly self-referential and unique per module.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdint.h>

extern "C" void *__dso_handle;

// Itanium ABI requires __dso_handle to be a self-referential pointer.
// This allows __cxa_atexit to identify which DSO registered a destructor.

int main() {
  // CHECK: DSO handle test
  printf("DSO handle test\n");

  // __dso_handle should point to itself.
  // CHECK: __dso_handle address = 0x
  printf("__dso_handle address = %p\n", &__dso_handle);

  // CHECK: __dso_handle value = 0x
  printf("__dso_handle value = %p\n", __dso_handle);

  // CHECK: self-referential = 1
  int self_ref = (__dso_handle == &__dso_handle);
  printf("self-referential = %d\n", self_ref);

  // __dso_handle should be in a valid memory region (not null).
  // CHECK: not null = 1
  printf("not null = %d\n", __dso_handle != NULL);

  // Address should be in module's address space.
  // On Windows, user-mode addresses are typically below 0x8000000000000000.
  uintptr_t addr = (uintptr_t)__dso_handle;
#ifdef _WIN64
  int valid_address = (addr > 0x10000 && addr < 0x8000000000000000ULL);
#else
  int valid_address = (addr > 0x10000 && addr < 0x80000000UL);
#endif
  // CHECK: valid address range = 1
  printf("valid address range = %d\n", valid_address);

  // CHECK: PASS
  if (self_ref) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL\n");
    return 1;
  }
}
