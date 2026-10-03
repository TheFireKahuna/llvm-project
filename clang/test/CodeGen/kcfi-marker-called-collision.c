// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// Two call types that share a check identifier because pointer generalisation
/// erases the difference between their parameters open independently. A cast
/// opens get_a, so a call through it hands back the function pointers that
/// struct ops_a holds, but a call through get_b, which has the same check
/// identifier and a different precise identifier, hands back nothing: its
/// types are given to the linker as a __kcfi_tinflow_ fact keyed by get_b's
/// precise identifier, and get_a's opening does not match it. The object
/// publishes the precise type it opens as __kcfi_popen_.

typedef int (*cb_a)(char *);
typedef int (*cb_b)(int *);
struct ops_a {
  cb_a f;
};
struct ops_b {
  cb_b f;
};
typedef struct ops_a *(*get_a)(char *);
typedef struct ops_b *(*get_b)(int *);

// CHECK:      module asm
/// Only get_b, which the object does not open, gives a tinflow fact, keyed by
/// its precise identifier; it names cb_b, which struct ops_b holds.
// CHECK-NEXT: ".weak __kcfi_tinflow_{{[0-9a-f]+}}_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_tinflow_{{[0-9a-f]+}}_{{[0-9a-f]+}}, {{[0-9]+}}"
/// get_a's opening and cb_a's opening are published as precise types.
// CHECK-NEXT: ".weak __kcfi_popen_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_popen_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_popen_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_popen_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NOT:  __kcfi_

int use(void *h) {
  get_a ga = (get_a)h;
  struct ops_a *a = ga("x");
  get_b gb = 0;
  struct ops_b *b = gb(0);
  return a->f("y") + b->f(0);
}

/// cb_a opens, from the call through get_a; cb_b does not.
// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#CBA:]]
int w_a(char *p) { return *p; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#CBB:]]
int w_b(int *p) { return *p; }

/// Exactly two types open: get_a's and cb_a's. cb_b (w_b) is absent, so a call
/// through get_b opened nothing.
// CHECK: !kcfi.dynamic = !{![[#]], ![[#CBA]]}
