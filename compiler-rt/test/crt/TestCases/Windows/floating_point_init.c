// Test floating-point state initialization.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <float.h>
#include <math.h>
#include <stdio.h>

// _fltused symbol indicates floating-point code is present.
extern int _fltused;

int main(void) {
  // CHECK: Floating-point initialization test
  printf("Floating-point initialization test\n");

  // _fltused should have MSVC magic value.
  // CHECK: _fltused = 0x9875
  printf("_fltused = 0x%x\n", _fltused);

  // Basic floating-point operations should work.
  double a = 3.14159265358979;
  double b = 2.71828182845904;
  double sum = a + b;
  double product = a * b;

  // CHECK: sum valid = 1
  int sum_ok = fabs(sum - 5.85987448204883) < 0.0001;
  printf("sum valid = %d\n", sum_ok);

  // CHECK: product valid = 1
  int product_ok = fabs(product - 8.53973422267356) < 0.0001;
  printf("product valid = %d\n", product_ok);

  // Test some math functions.
  double sq = sqrt(2.0);
  // CHECK: sqrt(2) valid = 1
  printf("sqrt(2) valid = %d\n", fabs(sq - 1.41421356237) < 0.0001);

  double sn = sin(1.0);
  // CHECK: sin(1) valid = 1
  printf("sin(1) valid = %d\n", fabs(sn - 0.84147098480789) < 0.0001);

  // Test denormals/subnormals work.
  volatile double tiny = DBL_MIN / 2.0;
  // CHECK: denormal preserved = 1
  printf("denormal preserved = %d\n", tiny > 0 && tiny < DBL_MIN);

  // Test infinity.
  volatile double inf = 1.0 / 0.0;
  // CHECK: infinity works = 1
  printf("infinity works = %d\n", isinf(inf));

  // Test NaN.
  volatile double nan_val = 0.0 / 0.0;
  // CHECK: NaN works = 1
  printf("NaN works = %d\n", isnan(nan_val));

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
