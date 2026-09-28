#pragma clang system_header

constexpr auto SystemChar = L'x';
constexpr auto &SystemString = L"ab" "c";

#define SYSTEM_WCHAR L'y'
#define SYSTEM_WSTRING L"de"

#define SYSTEM_TEXT_(q) L##q
#define SYSTEM_TEXT(q) SYSTEM_TEXT_(q)
