// Test security cookie properties in detail.
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
typedef uint64_t cookie_t;
#else
extern uint32_t __security_cookie;
extern uint32_t __security_cookie_complement;
#define DEFAULT_COOKIE 0xBB40E64EUL
#define COOKIE_FMT "0x%08lx"
typedef uint32_t cookie_t;
#endif

// Cookie check function provided by wincrt.
void __security_check_cookie(cookie_t cookie);

int main(void) {
  // CHECK: Security cookie properties test
  printf("Security cookie properties test\n");

  cookie_t cookie = __security_cookie;
  cookie_t complement = __security_cookie_complement;

  printf("__security_cookie = " COOKIE_FMT "\n", (unsigned long long)cookie);
  printf("__security_cookie_complement = " COOKIE_FMT "\n",
         (unsigned long long)complement);

  // Property 1: Cookie was initialized (not default).
  // CHECK: initialized (not default) = 1
  int initialized = (cookie != DEFAULT_COOKIE);
  printf("initialized (not default) = %d\n", initialized);

  // Property 2: Cookie is not zero.
  // CHECK: nonzero = 1
  int nonzero = (cookie != 0);
  printf("nonzero = %d\n", nonzero);

  // Property 3: Complement is correct (~cookie).
  // CHECK: complement correct = 1
  int complement_ok = (complement == ~cookie);
  printf("complement correct = %d\n", complement_ok);

#ifdef _WIN64
  // Property 4 (64-bit): Top 16 bits are zero (48-bit effective cookie).
  // This is for string overflow defense - the high bits would be overwritten
  // by null terminators in a string copy attack.
  // CHECK: top 16 bits zero = 1
  int top_bits_zero = ((cookie & 0xFFFF000000000000ULL) == 0);
  printf("top 16 bits zero = %d\n", top_bits_zero);
#else
  // Property 4 (32-bit): High word is non-zero.
  // Prevents accidental match with common small values.
  // CHECK: high word nonzero = 1
  int high_word_ok = ((cookie & 0xFFFF0000UL) != 0);
  printf("high word nonzero = %d\n", high_word_ok);
#endif

  // Property 5: Cookie check function accepts correct cookie.
  // (Would crash if incorrect - just verify we survive the call.)
  __security_check_cookie(cookie);
  // CHECK: cookie check passed = 1
  printf("cookie check passed = 1\n");

  // Property 6: Cookie is reasonably random (not a simple pattern).
  // Check that it's not just all 1s, all 0s, or a simple repeat.
  // CHECK: appears random = 1
  int appears_random = 1;
#ifdef _WIN64
  // Check for some bit diversity in lower 48 bits.
  uint64_t lower48 = cookie & 0x0000FFFFFFFFFFFFULL;
  uint32_t lo = (uint32_t)lower48;
  uint32_t hi = (uint32_t)(lower48 >> 32);
  if (lo == hi)
    appears_random = 0; // Too repetitive.
  if (lo == 0 || lo == 0xFFFFFFFF)
    appears_random = 0;
#else
  uint16_t lo = (uint16_t)cookie;
  uint16_t hi = (uint16_t)(cookie >> 16);
  if (lo == hi)
    appears_random = 0;
  if (lo == 0 || lo == 0xFFFF)
    appears_random = 0;
#endif
  printf("appears random = %d\n", appears_random);

  // CHECK: PASS
  if (initialized && nonzero && complement_ok) {
    printf("PASS\n");
    return 0;
  } else {
    printf("FAIL\n");
    return 1;
  }
}
