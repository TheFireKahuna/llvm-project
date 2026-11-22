// overridable_default takes a slot of the override table, from 0 to 31, then
// the slot of the default that this one is specified as calling, then whether
// that one takes this one's arguments without the second, which is checked
// against the default of that slot where the unit declares it. A function has
// one such statement. The attribute is of the 64-bit Windows Itanium and NT-POSIX
// targets.

// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-ntposix -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify=other -DOTHER %s
// RUN: %clang_cc1 -triple i686-unknown-windows-itanium -fsyntax-only -verify=other -DOTHER %s

__attribute__((overridable_default(0))) void a(); // other-warning {{unknown attribute 'overridable_default' ignored}}
[[clang::overridable_default(31, 0, true)]] void b(); // other-warning {{unknown attribute 'clang::overridable_default' ignored}}
#ifndef OTHER
__attribute__((overridable_default)) void c(); // expected-error {{'overridable_default' attribute takes at least 1 argument}}
__attribute__((overridable_default(0, 1, 1, 1))) void d(); // expected-error {{'overridable_default' attribute takes no more than 3 arguments}}
__attribute__((overridable_default(32))) void e(); // expected-error {{'overridable_default' attribute requires integer constant between 0 and 31 inclusive}}
__attribute__((overridable_default(1, 32))) void f(); // expected-error {{'overridable_default' attribute requires integer constant between 0 and 31 inclusive}}
__attribute__((overridable_default(1, 2, 2))) void g(); // expected-error {{'overridable_default' attribute requires integer constant between 0 and 1 inclusive}}
__attribute__((overridable_default(1, 1))) void h(); // expected-error {{an overridable default cannot forward to its own slot}}
__attribute__((overridable_default("x"))) void i(); // expected-error {{'overridable_default' attribute requires parameter 1 to be an integer constant}}
__attribute__((overridable_default(0))) int j; // expected-error {{'overridable_default' attribute only applies to functions}}

// A redeclaration repeats the statement or leaves it out.
__attribute__((overridable_default(3, 2))) void k();
__attribute__((overridable_default(3, 2))) void k(); // expected-note {{conflicting attribute is here}}
void k();
__attribute__((overridable_default(3))) void k(); // expected-error {{'overridable_default' and 'overridable_default' attributes are not compatible}}
__attribute__((overridable_default(5), overridable_default(4))) void l(); // expected-error {{'overridable_default' and 'overridable_default' attributes are not compatible}} expected-note {{conflicting attribute is here}}

// A forward is to a default that takes this one's arguments.
__attribute__((overridable_default(10))) void t(int, long);
__attribute__((overridable_default(11, 10))) void u(int, long, char);
__attribute__((overridable_default(12, 10, 1))) void v(int, char, long);
__attribute__((overridable_default(13, 10))) void w(int); // expected-error {{'overridable_default' attribute references parameter 2, but the function 'w' has only 1 parameters}}
__attribute__((overridable_default(14, 10, 1))) void x(int, char, int); // expected-error {{'overridable_default' attribute parameter types do not match: parameter 3 of function 'x' has type 'int', but parameter 2 of function 't' has type 'long'}}
__attribute__((overridable_default(15, 16))) void y(int);
__attribute__((overridable_default(16))) void z(char); // expected-error {{'overridable_default' attribute parameter types do not match: parameter 1 of function 'y' has type 'int', but parameter 1 of function 'z' has type 'char'}}
#endif
