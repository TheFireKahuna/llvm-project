// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,NOIFUNC
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=STUB
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,IFUNC

/// Without ifuncs the resolver is the multiversioned function's own symbol,
/// so a call through its address checks the resolver's type. An ifunc's
/// resolver returns a pointer and is never called through one. On Windows
/// Itanium an ifunc is a stub, whose address a call through it checks, so the
/// ifunc carries the type and its resolver none.

// IFUNC: @tc = weak_odr ifunc i32 (i32), ptr @tc.resolver
// IFUNC: @cd.ifunc = weak_odr ifunc i32 (i32), ptr @cd.resolver

// STUB: @tc = weak_odr dso_local ifunc i32 (i32), ptr @tc.resolver, !kcfi_type ![[#TYPE:]]
// STUB: @cd.ifunc = weak_odr dso_local ifunc i32 (i32), ptr @cd.resolver, !kcfi_type ![[#TYPE]]

// CHECK: define {{.*}}i32 @tc.default{{[.0-9]*}}(i32 {{.*}}) {{.*}}!kcfi_type ![[#TYPE:]]
// STUB:  define {{.*}}i32 @tc.default{{[.0-9]*}}(i32 {{.*}}) {{.*}}!kcfi_type ![[#TYPE]]
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
// STUB:  define weak_odr hidden ptr @tc.resolver() #{{[0-9]+}} comdat !resolver_fallback ![[#]] {
// STUB:  define weak_odr hidden ptr @cd.resolver() #{{[0-9]+}} comdat !resolver_fallback ![[#]] {

// CHECK: ![[#TYPE]] = !{i32 [[#]]}
// STUB:  ![[#TYPE]] = !{i32 [[#]]}
