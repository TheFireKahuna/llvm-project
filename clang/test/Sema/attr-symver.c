// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-apple-macosx -fsyntax-only -verify=macho %s

int f(void) __attribute__((symver("f@V1"))); // macho-warning {{unknown attribute 'symver' ignored}}
int g(void) __attribute__((symver("g"))); // expected-error {{'symver' attribute requires a name of the form 'name@version'}} \
                                          // macho-warning {{unknown attribute 'symver' ignored}}
int h(void) __attribute__((symver("@V1"))); // expected-error {{'symver' attribute requires a name of the form 'name@version'}} \
                                            // macho-warning {{unknown attribute 'symver' ignored}}
int i(void) __attribute__((symver(1))); // expected-error {{expected string literal as argument of 'symver' attribute}} \
                                        // macho-error {{expected string literal as argument of 'symver' attribute}}
struct S { int x; } __attribute__((symver("S@V1"))); // expected-warning {{'symver' attribute only applies to variables with non-local storage and functions}} \
                                                     // macho-warning {{unknown attribute 'symver' ignored}}
