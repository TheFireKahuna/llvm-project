// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++20 -fsyntax-only -verify %s

// expected-no-diagnostics

// A Win32 region's types are distinct from their standard twins, int,
// unsigned int, double and char16_t, and interchangeable with them in use.
#include "Inputs/ntposix-win32/region.h"

template <class T, class U> constexpr bool Same = false;
template <class T> constexpr bool Same<T, T> = true;

static_assert(!Same<LONG, int> && !Same<LONG, long>);
static_assert(!Same<WCHAR, char16_t> && !Same<WCHAR, wchar_t>);
static_assert(sizeof(LONG) == 4 && sizeof(long) == 8);
static_assert(Same<decltype(RegionString), const WCHAR *const>);

// The region's own overloads and specializations stay distinct.
static_assert(overload(ULONG()) == 2 && overload(0u) == 1);
static_assert(Traits<LONG>::Value == 2 && Traits<int>::Value == 1);
static_assert(wide(WCHAR()) == 2 && wide(u'a') == 1);

// A region value picks its twin's overload over every conversion.
constexpr int pick(int) { return 1; }
constexpr int pick(long) { return 2; }
constexpr int pick(long long) { return 3; }
static_assert(pick(LONG()) == 1);
constexpr int pickf(float) { return 1; }
constexpr int pickf(double) { return 2; }
constexpr int pickf(long double) { return 3; }
static_assert(pickf(LDOUBLE()) == 2);

// An lvalue binds to a reference to its twin, below an exact binding.
constexpr int bind(int &) { return 1; }
static_assert([] { LONG l = 0; return bind(l); }() == 1);
constexpr int bind2(int &) { return 1; }
constexpr int bind2(LONG &) { return 2; }
static_assert([] { LONG l = 0; return bind2(l); }() == 2);
static_assert([] { int i = 0; return bind2(i); }() == 1);

// A pointer converts to a pointer to its twin.
int *to_int(LONG *p) { return p; }
LONG *to_long(int *p) { return p; }
char16_t *to_char16(WCHAR *p) { return p; }

// A specialization only the twin has serves a region type; an exact one wins.
template <class T> struct Hash { static constexpr int Value = 0; };
template <> struct Hash<unsigned> { static constexpr int Value = 1; };
static_assert(Hash<ULONG>::Value == 1);
static_assert(Same<Hash<ULONG>, Hash<unsigned>>);
template <class T, class C> struct Formatter { static constexpr int Value = 0; };
template <class C> struct Formatter<int, C> { static constexpr int Value = 1; };
static_assert(Formatter<LONG, char>::Value == 1);
static_assert(Traits<LONG>::Value == 2);
