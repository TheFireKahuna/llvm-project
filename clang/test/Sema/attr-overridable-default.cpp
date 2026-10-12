// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fsyntax-only -verify=other %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify=other %s

typedef decltype(sizeof(0)) size_t;

// The slot is a constant that keeps the table within the executable's first
// page. Other targets have no such table.
[[clang::overridable_default(0)]] void *f0(size_t); // other-warning {{unknown attribute 'clang::overridable_default' ignored}}
[[clang::overridable_default(500)]] void f1(); // other-warning {{unknown attribute}}
__attribute__((overridable_default(4))) void f2(const char *, ...); // other-warning {{unknown attribute}}

[[clang::overridable_default(501)]] void f3(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 500 inclusive}} other-warning {{unknown attribute}}
[[clang::overridable_default]] void f4(); // expected-error {{'clang::overridable_default' attribute takes one argument}} other-warning {{unknown attribute}}
[[clang::overridable_default(-1)]] void f5(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 500 inclusive}} other-warning {{unknown attribute}}
int v [[clang::overridable_default(1)]]; // expected-error {{'clang::overridable_default' attribute only applies to functions}} other-warning {{unknown attribute}}
[[clang::overridable_default(2), clang::overridable_default(3)]] void f6(); // expected-error {{'clang::overridable_default' and 'clang::overridable_default' attributes are not compatible}} expected-note {{conflicting attribute is here}} other-warning 2 {{unknown attribute}}
