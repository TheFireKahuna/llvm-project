// On Windows Itanium and NT-POSIX a guard variable is in its variable's
// COMDAT, and so is the initialization function of a variable with vague
// linkage, for a thread-local variable the one of its own that _ZTH names:
// COFF makes their sections associative to the variable's. Other COFF targets
// give a guard variable a COMDAT of its own.

// RUN: %clang_cc1 -emit-llvm -triple x86_64-unknown-windows-itanium -std=c++17 %s -o - | FileCheck %s
// RUN: %clang_cc1 -emit-llvm -triple aarch64-unknown-windows-ntposix -std=c++17 %s -o - | FileCheck %s
// RUN: %clang_cc1 -emit-llvm -triple x86_64-windows-gnu -std=c++17 %s -o - | FileCheck %s --check-prefix=GNU

int f();

inline int v = f();
template <typename T> struct S {
  static thread_local int t;
};
template <typename T> thread_local int S<T>::t = f();
inline int &local() {
  static int l = f();
  return l;
}
int use() { return v + S<int>::t + local(); }

// CHECK-DAG: @_ZGV1v = linkonce_odr {{.*}}global i64 0, comdat($v)
// CHECK-DAG: @_ZGVN1SIiE1tE = linkonce_odr {{.*}}thread_local global i64 0, comdat($_ZN1SIiE1tE)
// CHECK-DAG: @_ZGVZ5localvE1l = linkonce_odr {{.*}}global i64 0, comdat($_ZZ5localvE1l)
// CHECK-DAG: @_ZTHN1SIiE1tE = linkonce_odr {{.*}}alias void (), ptr @[[INIT:__cxx_global_var_init[.0-9]*]]
// CHECK-DAG: define internal void @[[INIT]]() {{.*}}comdat($_ZN1SIiE1tE)
// CHECK-DAG: define internal void @__cxx_global_var_init{{[.0-9]*}}() {{.*}}comdat($v)

// GNU-DAG: @_ZGV1v = linkonce_odr {{.*}}global i64 0, comdat, align 8
// GNU-DAG: @_ZGVZ5localvE1l = linkonce_odr {{.*}}global i64 0, comdat, align 8
// GNU-NOT: comdat($_ZN1SIiE1tE)
