// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -std=c++17 \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -std=c++17 \
// RUN:   -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -std=c++17 -O1 \
// RUN:   -S -o %t.s %s
// RUN: FileCheck --check-prefix=ASM --input-file=%t.s %s
// RUN: FileCheck --check-prefix=INIT --input-file=%t.s %s

/// A variable with vague linkage and default visibility has one instance along
/// import paths, as on ELF: it is not dso_local, so code reaches it through
/// its import pointer, and neither is its guard. Its dynamic initializer is in
/// its COMDAT, so that the linker discards it with a copy that a DLL's
/// instance preempts, and the variable is kept through the link as the key of
/// its initializer. A thread-local one is reached through its record alike.
/// An alias of it is in its COMDAT and is reached the same way. Hidden
/// variables and aliases, and definitions with unique linkage, stay dso_local.

int f();
struct S { int n; S() : n(f()) {} };

inline S s;
inline int iv = 3;
template <class T> struct A { static S m; };
template <class T> S A<T>::m;
inline int *loc() { static int x = f(); return &x; }
inline thread_local int tls = 4;
__attribute__((visibility("hidden"))) inline int hidden = 5;
int strong = 6;
extern int ivalias __attribute__((alias("iv")));
extern int hiddenalias __attribute__((alias("iv"), visibility("hidden")));

int use() {
  return s.n + iv + A<int>::m.n + *loc() + tls + hidden + strong + ivalias +
         hiddenalias;
}

// CHECK-DAG: @s = linkonce_odr global %struct.S zeroinitializer, comdat
// CHECK-DAG: @_ZGV1s = linkonce_odr global i64 0, comdat
// CHECK-DAG: @iv = linkonce_odr global i32 3, comdat
// CHECK-DAG: @_ZN1AIiE1mE = linkonce_odr global %struct.S zeroinitializer, comdat
// CHECK-DAG: @_ZGVN1AIiE1mE = linkonce_odr global i64 0, comdat
// CHECK-DAG: @_ZZ3locvE1x = linkonce_odr global i32 0, comdat
// CHECK-DAG: @_ZGVZ3locvE1x = linkonce_odr global i64 0, comdat
// CHECK-DAG: @tls = linkonce_odr thread_local global i32 4, comdat
// CHECK-DAG: @hidden = linkonce_odr hidden global i32 5, comdat
// CHECK-DAG: @strong = dso_local global i32 6
// CHECK-DAG: @ivalias = alias i32, ptr @iv
// CHECK-DAG: @hiddenalias = hidden alias i32, ptr @iv
// CHECK-DAG: @llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @__cxx_global_var_init, ptr @s }, { i32, ptr, ptr } { i32 65535, ptr @__cxx_global_var_init.1, ptr @_ZN1AIiE1mE }]
// CHECK-DAG: @llvm.used = appending global [2 x ptr] [ptr @s, ptr @_ZN1AIiE1mE]

// CHECK-DAG: define internal void @__cxx_global_var_init() {{.*}}comdat($s)
// CHECK-DAG: define internal void @__cxx_global_var_init.1() {{.*}}comdat($_ZN1AIiE1mE)

// ASM-LABEL: _Z3usev:
// ASM-DAG:   movq __imp_s(%rip),
// ASM-DAG:   movq __imp_iv(%rip),
// ASM-DAG:   movq __imp__ZN1AIiE1mE(%rip),
// ASM-DAG:   movq __imp__ZGVZ3locvE1x(%rip),
// ASM-DAG:   movq __imp_tls$tls(%rip),
// ASM-DAG:   addl hidden(%rip),
// ASM-DAG:   addl strong(%rip),
// ASM-DAG:   movq __imp_ivalias(%rip),
// ASM-DAG:   addl hiddenalias(%rip),

// INIT:      .section .text,"xr",associative,s
// INIT-NOT:  .section
// INIT:      __cxx_global_var_init:
// INIT:      .section .CRT$XCU,"dr",associative,s
// INIT-NOT:  .section
// INIT:      .quad __cxx_global_var_init
