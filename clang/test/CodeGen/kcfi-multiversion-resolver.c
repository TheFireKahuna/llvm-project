// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,NOIFUNC
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,NOIFUNC
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,IFUNC

/// Without ifuncs the resolver is the multiversioned function's own symbol,
/// so a call through its address checks the resolver's type. An ifunc's
/// resolver returns a pointer and is never called through one.

// IFUNC: @tc = weak_odr ifunc i32 (i32), ptr @tc.resolver
// IFUNC: @cd.ifunc = weak_odr ifunc i32 (i32), ptr @cd.resolver

// CHECK: define {{.*}}i32 @tc.default{{[.0-9]*}}(i32 {{.*}}) {{.*}}!kcfi_type ![[#TYPE:]]
__attribute__((target_clones("avx2", "default"))) int tc(int x) { return x; }

__attribute__((cpu_specific(generic))) int cd(int x) { return x; }
__attribute__((cpu_specific(haswell))) int cd(int x) { return x + 1; }
__attribute__((cpu_dispatch(generic, haswell))) int cd(int x);

int (*ptc)(int) = tc;
int call_cd(int x) { return cd(x); }

// NOIFUNC: define weak_odr {{.*}}i32 @tc(i32 {{.*}}) {{.*}}!kcfi_type ![[#TYPE]]
// NOIFUNC: define weak_odr {{.*}}i32 @cd(i32 {{.*}}) {{.*}}!kcfi_type ![[#TYPE]]

// IFUNC: define weak_odr ptr @tc.resolver() #{{[0-9]+}} comdat {
// IFUNC: define weak_odr ptr @cd.resolver() #{{[0-9]+}} comdat {

// CHECK: ![[#TYPE]] = !{i32 [[#]]}
