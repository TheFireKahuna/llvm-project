// Test CFG symbol definitions for Windows loader patching.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

extern void* __guard_check_icall_fptr;
extern void* __guard_dispatch_icall_fptr;
extern uint32_t __guard_flags;

int main() {
  printf("CFG symbols test\n");

  printf("__guard_check_icall_fptr address = %p\n",
         (void*)&__guard_check_icall_fptr);
  printf("__guard_dispatch_icall_fptr address = %p\n",
         (void*)&__guard_dispatch_icall_fptr);
  printf("__guard_flags = 0x%08x\n", __guard_flags);

  int check_fptr_exists = (&__guard_check_icall_fptr != NULL);
  // CHECK: check_fptr exists = 1
  printf("check_fptr exists = %d\n", check_fptr_exists);

  int dispatch_fptr_exists = (&__guard_dispatch_icall_fptr != NULL);
  // CHECK: dispatch_fptr exists = 1
  printf("dispatch_fptr exists = %d\n", dispatch_fptr_exists);

  // 0x500 = IMAGE_GUARD_CF_INSTRUMENTED | IMAGE_GUARD_CF_FUNCTION_TABLE_PRESENT
  int flags_valid = ((__guard_flags & 0x500) == 0x500);
  // CHECK: flags valid = 1
  printf("flags valid = %d\n", flags_valid);

  printf("__guard_check_icall_fptr value = %p\n", __guard_check_icall_fptr);
  printf("__guard_dispatch_icall_fptr value = %p\n", __guard_dispatch_icall_fptr);

  // CHECK: PASS
  if (check_fptr_exists && dispatch_fptr_exists && flags_valid) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL\n");
    return 1;
  }
}
