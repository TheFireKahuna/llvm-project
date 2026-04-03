// Test that compiler-rt builtins work correctly on Windows Itanium.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

// Force use of builtins by using types that require runtime support.
// 128-bit integer operations require builtins on x86_64.

int main(void) {
  // CHECK: Builtins test
  printf("Builtins test\n");

  // Test 64-bit division (may use __divdi3/__udivdi3 on some configurations).
  int64_t a = 1000000000000LL;
  int64_t b = 12345LL;
  int64_t quot = a / b;
  int64_t rem = a % b;
  // CHECK: 64-bit div: 81004 rem 10180
  printf("64-bit div: %lld rem %lld\n", (long long)quot, (long long)rem);

  // Test unsigned 64-bit division.
  uint64_t ua = 0xFFFFFFFFFFFFFFFFULL;
  uint64_t ub = 0x100000000ULL;
  uint64_t uquot = ua / ub;
  // CHECK: 64-bit udiv: 4294967295
  printf("64-bit udiv: %llu\n", (unsigned long long)uquot);

  // Test 64-bit multiplication overflow detection.
  // __mulodi4 is used for overflow-checked multiplication.
  int64_t x = 0x7FFFFFFFLL;
  int64_t y = 0x100000000LL;
  int64_t product = x * y;
  // CHECK: 64-bit mul: 9223372032559808512
  printf("64-bit mul: %lld\n", (long long)product);

  // Test count leading zeros (clz).
  unsigned int val = 0x00100000;
  int clz = __builtin_clz(val);
  // CHECK: clz(0x100000) = 11
  printf("clz(0x100000) = %d\n", clz);

  // Test count trailing zeros (ctz).
  int ctz = __builtin_ctz(val);
  // CHECK: ctz(0x100000) = 20
  printf("ctz(0x100000) = %d\n", ctz);

  // Test popcount.
  unsigned int bits = 0xF0F0F0F0;
  int pop = __builtin_popcount(bits);
  // CHECK: popcount(0xF0F0F0F0) = 16
  printf("popcount(0xF0F0F0F0) = %d\n", pop);

  // Test byte swap.
  uint32_t swapped = __builtin_bswap32(0x12345678);
  // CHECK: bswap32(0x12345678) = 0x78563412
  printf("bswap32(0x12345678) = 0x%x\n", swapped);

  uint64_t swapped64 = __builtin_bswap64(0x123456789ABCDEF0ULL);
  // CHECK: bswap64 = 0xf0debc9a78563412
  printf("bswap64 = 0x%llx\n", (unsigned long long)swapped64);

  // Test floating-point classification.
  double d = 1.0 / 0.0;  // Infinity
  // CHECK: isinf(1.0/0.0) = 1
  printf("isinf(1.0/0.0) = %d\n", __builtin_isinf(d) ? 1 : 0);

  double nan = 0.0 / 0.0;
  // CHECK: isnan(0.0/0.0) = 1
  printf("isnan(0.0/0.0) = %d\n", __builtin_isnan(nan) ? 1 : 0);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
