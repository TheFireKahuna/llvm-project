// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fno-builtin -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fno-builtin-memcpy -fno-builtin-memmove -emit-llvm -o - %s | FileCheck %s

/// Without builtins, memcpy and memmove are still the C library's copies: a
/// copy whose source is directly a value that may come from another image
/// gives the types of the function pointers its destination's record holds,
/// and the copy function itself gets no __kcfi_inflow_ facts.

// CHECK-NOT: {{__kcfi_(inflow|param|tinflow)_}}

typedef __SIZE_TYPE__ size_t;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);

struct a_ops {
  a_fn f;
};
struct b_ops {
  b_fn f;
};

void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
__attribute__((dllimport)) void *imp_get(void);

void copy(void) {
  struct a_ops a;
  struct b_ops b;
  memcpy(&a, imp_get(), sizeof a);
  memmove(&b, imp_get(), sizeof b);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]]}
