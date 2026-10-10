// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=WI
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c89 \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=WI
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=C99
// RUN: %clang_cc1 -triple x86_64-scei-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=C99

// On Windows Itanium a C inline function is emitted as MSVC emits it: a
// discardable ODR definition in each unit that uses it, whether or not the
// unit gives it an external definition, unless gnu_inline asks for GNU's
// rules. Elsewhere C's own inline semantics apply.

__inline int f(void) { return 1; }
extern __inline int g(void) { return 2; }
__attribute__((gnu_inline)) extern __inline int h(void) { return 3; }

int use(void) { return f() + g() + h(); }

// WI-DAG: define linkonce_odr {{.*}}i32 @f()
// WI-DAG: define linkonce_odr {{.*}}i32 @g()
// WI-DAG: declare {{.*}}i32 @h()
// C99-DAG: declare {{.*}}i32 @f()
// C99-DAG: define {{.*}}i32 @g()
// C99-DAG: declare {{.*}}i32 @h()
