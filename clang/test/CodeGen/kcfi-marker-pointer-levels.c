// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A conversion between pointer types lets code read or write a function
/// pointer untyped when, peeling one pointer level from both sides at a time,
/// one side reaches a function pointer while the other is still a pointer of
/// another type. That function pointer's type opens dynamically.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_d({{.*}} !kcfi_type ![[#D:]]
long w_d(long x) { return x; }
// CHECK-DAG: define {{.*}} @w_e({{.*}} !kcfi_type ![[#E:]]
float w_e(float x) { return x; }

void convert(void ***v3, b_fn **b2, void *v1, d_fn **d2, e_fn **e2) {
  /// Two levels on each side, to and from a function pointer.
  a_fn **a2 = (a_fn **)v3;
  void **p2 = (void **)b2;
  /// One side runs out of pointer levels first.
  c_fn **c2 = (c_fn **)v1;
  /// The same function pointer type at the same depth.
  const d_fn **cd2 = (const d_fn **)d2;
  (void)a2;
  (void)p2;
  (void)c2;
  (void)cd2;
  (void)e2;
}

/// C, D and E are not listed.
// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]]}
