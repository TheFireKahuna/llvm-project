// RUN: %clang_cc1 -std=c++17 -fsyntax-only -verify -fwide-char16-literals %s
// RUN: %clang_cc1 -std=c++17 -fsyntax-only -verify -DNO_FLAG %s

// expected-no-diagnostics

// Under -fwide-char16-literals a wide literal spelled in a system header is a
// char16_t literal, also where user code expands the system macro that spells
// or pastes it. A wide literal spelled or pasted in user code keeps wchar_t.
#include "Inputs/wide-char16-literals.h"

#ifdef NO_FLAG
using SystemCharType = wchar_t;
#else
using SystemCharType = char16_t;
#endif

static_assert(__is_same(decltype(SystemChar), const SystemCharType), "");
static_assert(__is_same(decltype(SystemString), const SystemCharType (&)[4]),
              "");
static_assert(__is_same(decltype(SYSTEM_WCHAR), SystemCharType), "");
static_assert(__is_same(decltype(SYSTEM_WSTRING), const SystemCharType (&)[3]),
              "");

// A literal that a system macro pastes from user code's argument, as the SDK's
// TEXT() does, counts as written by the system header.
static_assert(
    __is_same(decltype(SYSTEM_TEXT("hi")), const SystemCharType (&)[3]), "");
static_assert(__is_same(decltype(SYSTEM_TEXT_('z')), SystemCharType), "");

static_assert(__is_same(decltype(L'x'), wchar_t), "");
static_assert(__is_same(decltype(L"ab"), const wchar_t (&)[3]), "");

#define USER_WSTRING L"fg"
static_assert(__is_same(decltype(USER_WSTRING), const wchar_t (&)[3]), "");
#define USER_TEXT(q) L##q
static_assert(__is_same(decltype(USER_TEXT("fg")), const wchar_t (&)[3]), "");
