// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fsyntax-only -verify=other %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify=other %s

typedef decltype(sizeof(0)) size_t;

// The slot is a constant that keeps the table within the executable's header
// page, below its block. A default specified as calling another names that
// one's slot, and whether that one takes its arguments without the second.
// Other targets have no such table.
[[clang::overridable_default(0)]] void *f0(size_t); // other-warning {{unknown attribute 'clang::overridable_default' ignored}}
[[clang::overridable_default(240)]] void f1(); // other-warning {{unknown attribute}}
__attribute__((overridable_default(4))) void f2(const char *, ...); // other-warning {{unknown attribute}}
[[clang::overridable_default(1, 0)]] void *f7(size_t); // other-warning {{unknown attribute}}
[[clang::overridable_default(9, 7, true)]] void f8(void *, size_t, size_t); // other-warning {{unknown attribute}}

[[clang::overridable_default(241)]] void f3(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 240 inclusive}} other-warning {{unknown attribute}}
[[clang::overridable_default]] void f4(); // expected-error {{'clang::overridable_default' attribute takes at least 1 argument}} other-warning {{unknown attribute}}
[[clang::overridable_default(-1)]] void f5(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 240 inclusive}} other-warning {{unknown attribute}}
int v [[clang::overridable_default(1)]]; // expected-error {{'clang::overridable_default' attribute only applies to functions}} other-warning {{unknown attribute}}
[[clang::overridable_default(2), clang::overridable_default(3)]] void f6(); // expected-error {{'clang::overridable_default' and 'clang::overridable_default' attributes are not compatible}} expected-note {{conflicting attribute is here}} other-warning 2 {{unknown attribute}}
[[clang::overridable_default(2, 2)]] void f9(); // expected-error {{an overridable default cannot forward to its own slot}} other-warning {{unknown attribute}}
[[clang::overridable_default(2, 241)]] void f10(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 240 inclusive}} other-warning {{unknown attribute}}
[[clang::overridable_default(3, 2, 2)]] void f11(); // expected-error {{'clang::overridable_default' attribute requires integer constant between 0 and 1 inclusive}} other-warning {{unknown attribute}}
[[clang::overridable_default(3, 2), clang::overridable_default(3, 4)]] void f12(); // expected-error {{'clang::overridable_default' and 'clang::overridable_default' attributes are not compatible}} expected-note {{conflicting attribute is here}} other-warning 2 {{unknown attribute}}
[[clang::overridable_default(3, 2, 1, 0)]] void f13(); // expected-error {{'clang::overridable_default' attribute takes no more than 3 arguments}} other-warning {{unknown attribute}}
