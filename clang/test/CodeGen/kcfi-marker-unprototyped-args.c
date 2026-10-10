// RUN: %clang_cc1 -std=c17 -Wno-deprecated-non-prototype -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -std=c17 -Wno-deprecated-non-prototype -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A call to a function without a prototype says nothing of the types of its
/// parameters, so its arguments are untyped, as variadic ones are: the
/// function pointers held in an object that an argument points to open
/// dynamically for a known import and join the __kcfi_inflow_ facts of a C
/// declaration that is not one. A pointer to a const object gives nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);

/// Address-taken declarations give each type's value as __kcfi_typeid_; a
/// call to a declaration without a prototype takes its address.
short t_b(short);
__attribute__((used)) static b_fn take_b = t_b;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_b"
// CHECK-NEXT: ".set __kcfi_typeid_t_b, [[#%u,B:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_imp_fill"
// CHECK-NEXT: ".set __kcfi_typeid_imp_fill, {{.*}}"
// CHECK-NEXT: ".weak __kcfi_typeid_foreign_fill"
// CHECK-NEXT: ".set __kcfi_typeid_foreign_fill, {{.*}}"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,B]]_foreign_fill"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,B]]_foreign_fill, {{[0-9]+}}"
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

__attribute__((dllimport)) void imp_fill();
void foreign_fill();

void fill(void) {
  struct a_ops a;
  struct b_ops b;
  const struct c_ops c = {0};
  imp_fill(&a, &c);
  foreign_fill(&b);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]]}
