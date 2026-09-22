// RUN: %clang_cc1 -std=c++17 -fsyntax-only -verify -fwide-char16-literals %s
// RUN: %clang_cc1 -std=c++17 -fsyntax-only -verify -DNO_FLAG %s

// expected-no-diagnostics

// A wide literal in a system header is a char16_t literal under
// -fwide-char16-literals; one in user code keeps wchar_t.
#include "Inputs/wide-char16-literals.h"

#ifdef NO_FLAG
using SystemCharType = wchar_t;
#else
using SystemCharType = char16_t;
#endif

static_assert(__is_same(decltype(SystemChar), const SystemCharType), "");
static_assert(__is_same(decltype(SystemString), const SystemCharType (&)[4]), "");
static_assert(__is_same(decltype(L'x'), wchar_t), "");
static_assert(__is_same(decltype(L"ab"), const wchar_t (&)[3]), "");
