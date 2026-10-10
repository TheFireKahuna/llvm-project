// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -x c -emit-llvm -o - %s | FileCheck %s --check-prefix=C
// RUN: %clang_cc1 -triple x86_64-windows-msvc -fms-extensions -emit-llvm -o - %s | FileCheck %s --check-prefix=MSVC

// Static data that holds the address of a dllimport entity, or of a point
// inside it, is a constant with a relocation against the imported symbol,
// which the linker turns into a word the loader fills in place, or one the
// image writes before its initializers run: no initializer of the program's
// runs for it, and a constant so initialized folds like any other.

__declspec(dllimport) extern int imported_int;
__declspec(dllimport) extern int imported_array[4];
__declspec(dllimport) int imported_func(void);

int *pint = &imported_int;
int *pfirst = &imported_array[0];
int (*pfunc)(void) = imported_func;
int *const pconst = &imported_int;
int *use_const(void) { return pconst; }

// CHECK-DAG: @pint = dso_local global ptr @imported_int
// CHECK-DAG: @pfirst = dso_local global ptr @imported_array
// CHECK-DAG: @pfunc = dso_local global ptr @_Z13imported_funcv
// CHECK-DAG: @pmethod = dso_local global { i64, i64 } { i64 ptrtoint (ptr @_ZN3Foo6methodEv to i64), i64 0 }
// CHECK-DAG: @pelement = dso_local global ptr getelementptr (i8, ptr @imported_array, i64 8)
// CHECK-DAG: @imported_int = external dllimport global i32
// CHECK-DAG: declare dllimport {{.*}}i32 @_Z13imported_funcv()
// CHECK-LABEL: define dso_local {{.*}}ptr @_Z9use_constv()
// CHECK: ret ptr @imported_int
// CHECK-NOT: @__cxx_global_var_init

// C already used the thunk's address for a function; a data address is now
// a constant too instead of a compile error.
// C-DAG: @pint = dso_local global ptr @imported_int
// C-DAG: @pfirst = dso_local global ptr @imported_array
// C-DAG: @pfunc = dso_local global ptr @imported_func
// C-LABEL: define dso_local {{.*}}ptr @use_const()
// C: ret ptr @imported_int

// MSVC-DAG: @"?pint@@3PEAHEA" = dso_local global ptr null
// MSVC-DAG: @"?pfunc@@3P6AHXZEA" = dso_local global ptr null
// MSVC-DAG: @llvm.global_ctors

#ifdef __cplusplus
struct Foo {
  __declspec(dllimport) int method();
};
int (Foo::*pmethod)() = &Foo::method;
int *pelement = &imported_array[2];
#endif
