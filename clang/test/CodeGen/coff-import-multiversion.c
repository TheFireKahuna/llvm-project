// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -mdefault-visibility-export-mapping=explicit \
// RUN:   | FileCheck --check-prefix=EXPLICIT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -mdefault-visibility-export-mapping=all \
// RUN:   | FileCheck --check-prefix=ALL %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -fno-plt | FileCheck --check-prefix=NOPLT %s

/// A multiversioned function is an ifunc, whose stub every image reaches,
/// so the ifunc takes a definition's storage: exported when the mapping exports
/// the function, and never dllimport, under -fno-plt too. Its versions and its
/// resolver are the image's own.

#pragma GCC visibility push(default)
int __attribute__((target_clones("avx2", "default"))) marked(int x) {
  return x + 1;
}
#pragma GCC visibility pop

int __attribute__((target_clones("avx2", "default"))) plain(int x) {
  return x + 2;
}

int use(int x) { return marked(x) + plain(x); }

// EXPLICIT-DAG: @marked = weak_odr dso_local dllexport ifunc i32 (i32), ptr @marked.resolver
// EXPLICIT-DAG: @plain = weak_odr dso_local ifunc i32 (i32), ptr @plain.resolver
// ALL-DAG:      @marked = weak_odr dso_local dllexport ifunc i32 (i32), ptr @marked.resolver
// ALL-DAG:      @plain = weak_odr dso_local dllexport ifunc i32 (i32), ptr @plain.resolver
// NOPLT-DAG:    @marked = weak_odr dso_local ifunc i32 (i32), ptr @marked.resolver
// NOPLT-DAG:    @plain = weak_odr dso_local ifunc i32 (i32), ptr @plain.resolver

// EXPLICIT-DAG: define hidden i32 @marked.avx2.0(
// EXPLICIT-DAG: define hidden i32 @marked.default.1(
// EXPLICIT-DAG: define weak_odr hidden ptr @marked.resolver(
// ALL-DAG:      define hidden i32 @plain.avx2.0(
// ALL-DAG:      define weak_odr hidden ptr @plain.resolver(
