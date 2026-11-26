// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fsyntax-only -verify %s

// Where ifuncs serve only multiversioned functions, the attribute is not
// available, and a written one is an error.

#if __has_attribute(ifunc)
#error "ifunc is available"
#endif

void *resolve(void);
int f(void) __attribute__((ifunc("resolve")));
// expected-error@-1 {{ifunc 'f' needs a resolver cell, which is not supported on this target}}
