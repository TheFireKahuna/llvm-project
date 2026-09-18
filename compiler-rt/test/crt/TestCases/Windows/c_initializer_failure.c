// Test C initializer (.CRT$XI*) failure handling.
// A C initializer returning non-zero should abort startup.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: not %run %t.exe 2>&1 | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

typedef int (*_PIFV)(void);

// This initializer succeeds.
#pragma section(".CRT$XIB", long, read)
static int init_success(void) {
  fprintf(stderr, "init_success: returning 0\n");
  return 0;  // Success.
}
__attribute__((used))
__declspec(allocate(".CRT$XIB")) static _PIFV init_success_ptr = init_success;

// This initializer fails - should abort process.
#pragma section(".CRT$XIC", long, read)
static int init_failure(void) {
  fprintf(stderr, "init_failure: returning -1 (error)\n");
  return -1;  // Failure.
}
__attribute__((used))
__declspec(allocate(".CRT$XIC")) static _PIFV init_failure_ptr = init_failure;

// This initializer should never run.
#pragma section(".CRT$XID", long, read)
static int init_never(void) {
  fprintf(stderr, "init_never: THIS SHOULD NOT PRINT\n");
  return 0;
}
__attribute__((used))
__declspec(allocate(".CRT$XID")) static _PIFV init_never_ptr = init_never;

int main(void) {
  fprintf(stderr, "main: THIS SHOULD NOT PRINT\n");
  return 0;
}

// CHECK: init_success: returning 0
// CHECK: init_failure: returning -1 (error)
// CHECK-NOT: init_never
// CHECK-NOT: main
