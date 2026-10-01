// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -mdefault-visibility-export-mapping=explicit \
// RUN:   | FileCheck --check-prefix=EXPLICIT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   -mdefault-visibility-export-mapping=all \
// RUN:   | FileCheck --check-prefix=ALL %s

/// A call to a multiversioned function reaches its resolver, which is defined
/// only at the end of the translation unit, after the call has given its
/// declaration the storage of an import. The definition takes a definition's
/// storage instead: exported when the mapping exports the function, and
/// never dllimport.

#pragma GCC visibility push(default)
int __attribute__((target_clones("avx2", "default"))) marked(int x) {
  return x + 1;
}
#pragma GCC visibility pop

int __attribute__((target_clones("avx2", "default"))) plain(int x) {
  return x + 2;
}

int use(int x) { return marked(x) + plain(x); }

// EXPLICIT-DAG: define weak_odr dso_local dllexport i32 @marked(
// EXPLICIT-DAG: define weak_odr dso_local i32 @plain(
// ALL-DAG:      define weak_odr dso_local dllexport i32 @marked(
// ALL-DAG:      define weak_odr dso_local dllexport i32 @plain(
