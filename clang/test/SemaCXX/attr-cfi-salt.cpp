// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++17 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -std=c++17 -fsyntax-only -verify=msvc -DMS_ABI %s

// In C++ a salted function type is a distinct type, which the Itanium ABI
// mangles and the Microsoft ABI cannot.

#define __cfi_salt(S) __attribute__((cfi_salt(S)))

template <class T, class U> struct is_same { static const bool value = false; };
template <class T> struct is_same<T, T> { static const bool value = true; };

typedef void (*plain_t)(void *);
typedef void (*salted_t)(void *) __cfi_salt("pepper"); // msvc-error{{'cfi_salt' attribute is not supported in C++ with the Microsoft ABI}}
typedef void (*other_t)(void *) [[clang::cfi_salt("salt 'n")]]; // msvc-error{{'clang::cfi_salt' attribute is not supported in C++ with the Microsoft ABI}}

#ifndef MS_ABI
static_assert(!is_same<plain_t, salted_t>::value, "");
static_assert(!is_same<salted_t, other_t>::value, "");
typedef void (*salted2_t)(void *) __cfi_salt("pepper");
static_assert(is_same<salted_t, salted2_t>::value, "");

void salted_fn(void *) __cfi_salt("pepper");
void plain_fn(void *);

int f(plain_t);
long f(salted_t);
static_assert(is_same<decltype(f(salted_fn)), long>::value, "");
static_assert(is_same<decltype(f(plain_fn)), int>::value, "");

plain_t p1 = salted_fn; // expected-error{{cannot initialize a variable of type 'plain_t' (aka 'void (*)(void *)') with an lvalue of type 'void (void *)'}}
salted_t p2 = salted_fn;
salted_t p3 = plain_fn; // expected-error{{cannot initialize a variable of type 'salted_t' (aka 'void (*)(void *)') with an lvalue of type 'void (void *)'}}

void g(void *) __cfi_salt("pepper"); // expected-note{{previous declaration is here}}
void g(void *) {} // expected-error{{conflicting types for 'g'}}

auto lambda = [](void *) __cfi_salt("pepper") {};
salted_t p4 = lambda;
#endif
