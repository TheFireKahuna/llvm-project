#pragma clang win32_header

#ifndef _WIN32
#error a Win32 region sees _WIN32
#endif
#if WINVER != 0x0A00 || _WIN32_WINNT != 0x0A00
#error a Win32 region sees Windows 10's version
#endif

typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned long DWORD;
typedef long double LDOUBLE;
typedef wchar_t WCHAR;

_Static_assert(sizeof(long) == 4, "a region's long is 32 bits");
_Static_assert(sizeof(long double) == 8, "a region's long double is a double");
_Static_assert(sizeof(wchar_t) == 2, "a region's wchar_t is 16 bits");
_Static_assert(sizeof(L"ab") == 6, "a region's wide literal is UTF-16");
_Static_assert(sizeof(1L) == 4, "a region's L literal is its long");

#define REGION_TEXT_(q) L##q
#define REGION_TEXT(q) REGION_TEXT_(q)

LONG take_long(LONG *value);
