// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A copy by memcpy or memmove, or one of their builtin forms, whose source
/// is directly a value that may come from another image or from foreign code
/// gives the types of the function pointers its destination's record holds,
/// as a conversion of that value to a pointer to the record does: they open
/// dynamically for a call to a known import or a load of imported data, and
/// join the __kcfi_inflow_ facts of a C declaration that is not a known
/// import. A source that is not directly such a value, or is the result of a
/// call to a function the unit defines, gives nothing.

typedef __SIZE_TYPE__ size_t;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);

/// An address-taken declaration gives the type's value as __kcfi_typeid_.
int t_c(int);
__attribute__((used)) static c_fn take_c = t_c;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_c, [[#%u,C:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,C]]_foreign_get"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,C]]_foreign_get, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

struct a_ops {
  a_fn f;
};
struct b_ops {
  b_fn f;
};
struct c_ops {
  c_fn f;
};
struct d_ops {
  d_fn f;
};
struct e_ops {
  e_fn f;
};

void *memcpy(void *, const void *, size_t);
__attribute__((dllimport)) void *imp_get(void);
__attribute__((dllimport)) extern void *imp_data;
void *foreign_get(void);
void *mine(void) { return 0; }

void copy(void *local) {
  struct a_ops a;
  struct b_ops b;
  struct c_ops c;
  struct d_ops d;
  struct e_ops e;
  memcpy(&a, imp_get(), sizeof a);
  __builtin_memmove(&b, imp_data, sizeof b);
  __builtin_memcpy_inline(&c, foreign_get(), sizeof c);
  memcpy(&d, local, sizeof d);
  __builtin_memcpy(&e, mine(), sizeof e);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]]}
