// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s

// __builtin_overridable_default_slot takes exactly one function that is an
// overridable default.

typedef __SIZE_TYPE__ size_t;

__attribute__((overridable_default(3))) void *marked(size_t);
void *plain(size_t);
int notafunction;

unsigned ok() { return __builtin_overridable_default_slot(marked); }

unsigned missing_attr() {
  return __builtin_overridable_default_slot(plain); // expected-error {{argument must be a function that is an overridable default}}
}

unsigned not_a_function() {
  return __builtin_overridable_default_slot(notafunction); // expected-error {{argument must be a function that is an overridable default}}
}

unsigned too_many() {
  return __builtin_overridable_default_slot(marked, marked); // expected-error {{too many arguments}}
}

unsigned too_few() {
  return __builtin_overridable_default_slot(); // expected-error {{too few arguments}}
}
