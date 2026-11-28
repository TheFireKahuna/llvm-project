// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fms-extensions -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fms-extensions -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fsyntax-only -verify %s

// The thread_pointer attribute is a function declaration's, not its type's: a
// function's address is its plain entry, so a pointer to a function of the C
// convention takes one that derives its thread pointer, looks it up or has
// none. Windows's conventions stay types of their own on NT-POSIX.

int passed(int);
__attribute__((thread_pointer("derive"))) int derives(int);
__attribute__((thread_pointer("probe"))) int probes(int);
__attribute__((thread_pointer("none"))) int has_none(int);
__attribute__((thread_pointer("refusal"))) long reports(int);
__attribute__((thread_pointer("read"))) void *reads(unsigned long long);

typedef int (*fn)(int);
fn a = passed;
fn b = derives;
fn c = probes;
fn d = has_none;
long (*e)(int) = reports;

__attribute__((thread_pointer("sometimes"))) int unknown(int); // expected-warning {{'thread_pointer' attribute argument not supported: sometimes}}
typedef int (*typed)(int) __attribute__((thread_pointer("derive"))); // expected-error {{'thread_pointer' attribute only applies to functions}}
int not_a_function __attribute__((thread_pointer("derive"))); // expected-error {{'thread_pointer' attribute only applies to functions}}
