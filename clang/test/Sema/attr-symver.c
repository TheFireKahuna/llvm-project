// RUN: %clang_cc1 -triple x86_64-linux-gnu -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple arm64-apple-macosx -fsyntax-only -verify=macho %s

__attribute__((symver("f@V1"))) int ok1(void); // macho-warning {{unknown attribute 'symver' ignored}}
__attribute__((symver("f@@V2"))) int ok2;      // macho-warning {{unknown attribute 'symver' ignored}}

__attribute__((symver("f"))) int no_version(void);     // expected-error {{'symver' attribute requires a name of the form 'name@version' or 'name@@version'}} macho-warning {{ignored}}
__attribute__((symver("@V1"))) int no_name(void);      // expected-error {{'symver' attribute requires a name}} macho-warning {{ignored}}
__attribute__((symver("f@"))) int empty(void);         // expected-error {{'symver' attribute requires a name}} macho-warning {{ignored}}
__attribute__((symver("f@@@V1"))) int three(void);     // expected-error {{'symver' attribute requires a name}} macho-warning {{ignored}}
__attribute__((symver("f@V1@V2"))) int two(void);      // expected-error {{'symver' attribute requires a name}} macho-warning {{ignored}}
__attribute__((symver(1))) int not_string(void);       // expected-error {{expected string literal as argument of 'symver' attribute}} macho-error {{expected string literal}}
struct __attribute__((symver("s@V1"))) s { int m; };   // expected-warning {{'symver' attribute only applies to variables with non-local storage and functions}} macho-warning {{ignored}}
void fn(void) {
  __attribute__((symver("l@V1"))) int local;           // expected-warning {{'symver' attribute only applies to}} macho-warning {{ignored}}
}
