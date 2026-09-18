// Test security cookie initialization to a random value at CRT startup.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN64
extern uint64_t __security_cookie;
extern uint64_t __security_cookie_complement;
#define DEFAULT_COOKIE 0x00002B992DDFA232ULL
#define COOKIE_FMT "0x%016llx"
#else
extern uint32_t __security_cookie;
extern uint32_t __security_cookie_complement;
#define DEFAULT_COOKIE 0xBB40E64EUL
#define COOKIE_FMT "0x%08lx"
#endif

int main() {
  printf("Security cookie test\n");

  printf("__security_cookie = " COOKIE_FMT "\n",
         (unsigned long long)__security_cookie);
  printf("__security_cookie_complement = " COOKIE_FMT "\n",
         (unsigned long long)__security_cookie_complement);

  int cookie_initialized = (__security_cookie != DEFAULT_COOKIE);
  // CHECK: cookie initialized = 1
  printf("cookie initialized = %d\n", cookie_initialized);

  int cookie_nonzero = (__security_cookie != 0);
  // CHECK: cookie nonzero = 1
  printf("cookie nonzero = %d\n", cookie_nonzero);

  int complement_correct =
      (__security_cookie_complement == ~__security_cookie);
  // CHECK: complement correct = 1
  printf("complement correct = %d\n", complement_correct);

#ifdef _WIN64
  // Top 16 bits masked for string overflow defense.
  int top_bits_masked = ((__security_cookie & 0xFFFF000000000000ULL) == 0);
  // CHECK: top bits masked = 1
  printf("top bits masked = %d\n", top_bits_masked);
#endif

  // CHECK: PASS
  if (cookie_initialized && cookie_nonzero && complement_correct) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL\n");
    return 1;
  }
}
