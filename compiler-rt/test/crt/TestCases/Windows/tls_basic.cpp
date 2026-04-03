// Test basic __declspec(thread) TLS variables.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

__declspec(thread) int tls_var = 42;
__declspec(thread) int tls_var_zero = 0;

int main() {
  // CHECK: tls_var initial = 42
  printf("tls_var initial = %d\n", tls_var);

  // CHECK: tls_var_zero initial = 0
  printf("tls_var_zero initial = %d\n", tls_var_zero);

  tls_var = 100;
  tls_var_zero = 200;

  // CHECK: tls_var modified = 100
  printf("tls_var modified = %d\n", tls_var);

  // CHECK: tls_var_zero modified = 200
  printf("tls_var_zero modified = %d\n", tls_var_zero);

  // CHECK: TLS test passed
  printf("TLS test passed\n");
  return 0;
}
