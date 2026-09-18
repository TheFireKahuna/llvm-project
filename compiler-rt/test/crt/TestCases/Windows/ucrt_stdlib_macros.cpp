// Preserve application macros without leaking UCRT's C-only min/max macros.
// Exercise the actual SDK, Clang resource headers and libc++ in both orders.
// RUN: %clangxx_crt_main -std=c++17 -fsyntax-only -DMACROS=0 -D_CRT_DECLARE_NONSTDC_NAMES=1 %s
// RUN: %clangxx_crt_main -std=c++17 -fsyntax-only -DMACROS=1 -D_CRT_DECLARE_NONSTDC_NAMES=1 %s
// RUN: %clangxx_crt_main -std=c++17 -fsyntax-only -DMACROS=2 -D_CRT_DECLARE_NONSTDC_NAMES=1 %s
// RUN: %clangxx_crt_main -std=c++23 -fsyntax-only -DMACROS=3 -DINCLUDE_CXX_FIRST -D_CRT_DECLARE_NONSTDC_NAMES=1 %s
// RUN: %clangxx_crt_main -std=c++23 -fsyntax-only -DMACROS=4 -DINCLUDE_CXX_FIRST -D_CRT_DECLARE_NONSTDC_NAMES=1 %s
// RUN: %clangxx_crt_main -std=c++20 -fsyntax-only -DMACROS=1 -DINCLUDE_CXX_FIRST -DNOMINMAX -D_CRT_DECLARE_NONSTDC_NAMES=0 %s
// REQUIRES: windows, crt

#if MACROS == 1
#  define min(a, b) ((a) + 100)
#  define max(a, b) ((b) + 200)
#elif MACROS == 2
#  define min 17
#  define max 29
#elif MACROS == 3
#  define min(a, b) ((a) + 100)
#elif MACROS == 4
#  define max 29
#endif

// clang-format off
#ifdef INCLUDE_CXX_FIRST
#include <cstdlib>
#include <stdlib.h>
#else
#include <stdlib.h>
#include <cstdlib>
#endif
// clang-format on
#include <type_traits>

#if MACROS == 1
static_assert(min(2, 3) == 102 && max(2, 3) == 203);
#elif MACROS == 2
static_assert(min == 17 && max == 29);
#elif MACROS == 3
static_assert(min(2, 3) == 102);
#elif MACROS == 4
static_assert(max == 29);
#endif

#if (MACROS == 0 || MACROS == 4) && defined(min)
#  error min must remain undefined
#endif
#if (MACROS == 0 || MACROS == 3) && defined(max)
#  error max must remain undefined
#endif

// libc++ overloads remain usable with those macros in scope.
static_assert(std::is_same_v<decltype(std::abs(-1L)), long>);
static_assert(std::is_same_v<decltype(std::abs(-1LL)), long long>);
static_assert(std::is_same_v<decltype(std::div(7L, 3L)), std::ldiv_t>);
static_assert(std::is_same_v<decltype(std::div(7LL, 3LL)), std::lldiv_t>);
