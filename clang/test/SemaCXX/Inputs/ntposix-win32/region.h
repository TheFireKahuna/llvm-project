#pragma clang win32_header

typedef long LONG;
typedef unsigned long ULONG;
typedef long double LDOUBLE;
typedef wchar_t WCHAR;

// MSVC's identities: overloads and specializations on a type and its twin
// stay distinct.
constexpr int overload(unsigned) { return 1; }
constexpr int overload(unsigned long) { return 2; }

template <class T> struct Traits;
template <> struct Traits<int> { static constexpr int Value = 1; };
template <> struct Traits<long> { static constexpr int Value = 2; };

constexpr int wide(char16_t) { return 1; }
constexpr int wide(wchar_t) { return 2; }

constexpr auto RegionString = L"ab";
