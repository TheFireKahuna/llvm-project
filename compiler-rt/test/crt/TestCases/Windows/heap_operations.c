// Test basic heap operations via UCRT malloc/free.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  // CHECK: Heap operations test
  printf("Heap operations test\n");

  // Test malloc/free.
  int *p = (int *)malloc(sizeof(int));
  // CHECK: malloc succeeded = 1
  printf("malloc succeeded = %d\n", p != NULL);
  *p = 42;
  // CHECK: value = 42
  printf("value = %d\n", *p);
  free(p);

  // Test calloc (zero-initialized).
  int *arr = (int *)calloc(10, sizeof(int));
  // CHECK: calloc succeeded = 1
  printf("calloc succeeded = %d\n", arr != NULL);
  int all_zero = 1;
  for (int i = 0; i < 10; ++i) {
    if (arr[i] != 0) {
      all_zero = 0;
      break;
    }
  }
  // CHECK: calloc zeroed = 1
  printf("calloc zeroed = %d\n", all_zero);
  free(arr);

  // Test realloc.
  char *str = (char *)malloc(10);
  strcpy(str, "hello");
  str = (char *)realloc(str, 20);
  // CHECK: realloc preserved = 1
  printf("realloc preserved = %d\n", strcmp(str, "hello") == 0);
  strcat(str, " world");
  // CHECK: str = hello world
  printf("str = %s\n", str);
  free(str);

  // Test aligned allocation (C11 aligned_alloc via UCRT).
  void *aligned = _aligned_malloc(1024, 64);
  // CHECK: aligned_malloc succeeded = 1
  printf("aligned_malloc succeeded = %d\n", aligned != NULL);
  // CHECK: alignment correct = 1
  printf("alignment correct = %d\n", ((size_t)aligned % 64) == 0);
  _aligned_free(aligned);

  // Test large allocation.
  size_t large_size = 100 * 1024 * 1024;  // 100 MB
  void *large = malloc(large_size);
  // CHECK: large alloc succeeded = 1
  printf("large alloc succeeded = %d\n", large != NULL);
  // Write to verify.
  memset(large, 0xAB, large_size);
  // CHECK: large alloc usable = 1
  printf("large alloc usable = %d\n",
         ((char *)large)[0] == (char)0xAB &&
             ((char *)large)[large_size - 1] == (char)0xAB);
  free(large);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
