// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -O1 -S -o - %s | FileCheck --check-prefix=ASM %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -O1 -S -o - %s | FileCheck --check-prefix=ASM %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-obj -o /dev/null %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-obj -o /dev/null %s

// Callers reach a multiversioned function through its ifunc, which the
// resolver's mark describes: it hands the version it chooses the thread
// pointer, so its symbol carries "$tp", and code generation gives it a plain
// entry that calls it. No call forwards a variadic function's arguments, so
// that has none.

#ifdef __x86_64__
#define CLONES target_clones("avx512f", "default")
#else
#define CLONES target_clones("i8mm", "default")
#endif

__attribute__((CLONES)) int cloned(int x) { return x + 1; }
__attribute__((CLONES)) int vcloned(int x, ...) { return x; }
int use(int x) { return cloned(x) + vcloned(x, 1); }

// CHECK: @cloned = weak_odr dso_local ifunc i32 (i32), ptr @cloned.resolver
// CHECK: @vcloned = weak_odr dso_local ifunc i32 (i32, ...), ptr @vcloned.resolver
// CHECK: define weak_odr hidden ptr @cloned.resolver() #[[ARG:[0-9]+]] comdat
// CHECK: define weak_odr hidden ptr @vcloned.resolver() #[[ARG]] comdat
// CHECK-NOT: @"\01
// CHECK: attributes #[[ARG]] = { {{.*}}"thread-pointer-argument" }

// ASM: {{^}}use$tp:
// ASM: {{^}}cloned:
// ASM: {{call|bl}}{{q?}}{{[[:space:]]+}}cloned$tp
// ASM: {{^}}cloned$tp:
// ASM: {{^}}vcloned$tp:
// ASM-NOT: {{^}}vcloned:
