// RUN: %clang_cc1 -std=c++11 -fsyntax-only -verify %s
// RUN: %clang_cc1 -std=c++11 -fsyntax-only -verify %s \
// RUN:   -triple x86_64-unknown-windows-itanium

struct [[clang::no_abi_witness]] Extensible {
  unsigned size;
};
union [[clang::no_abi_witness]] U {
  int i;
};
class [[clang::no_abi_witness]] C;
class C {};

int i [[clang::no_abi_witness]]; // expected-warning {{'clang::no_abi_witness' attribute only applies to structs, unions, and classes}}
[[clang::no_abi_witness]] void f(); // expected-warning {{'clang::no_abi_witness' attribute only applies to}}
struct [[clang::no_abi_witness(1)]] Args {}; // expected-error {{'clang::no_abi_witness' attribute takes no arguments}}
