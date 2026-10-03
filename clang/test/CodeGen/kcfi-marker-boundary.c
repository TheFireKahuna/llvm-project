// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A conversion to a pointer to a record whose operand is directly the result
/// of a call or a load that may come from another image or from foreign code
/// gives the types of the function pointers the record holds: they open
/// dynamically for a call to a known import or a load of imported data, and
/// a call to a C declaration that is not a known import gives them to the
/// linker as __kcfi_inflow_ facts of that declaration. An operand that is not
/// directly such a value, a load of a variable that is not imported, a call
/// to a function the unit defines and a record without function pointers
/// give nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);
typedef long long (*g_fn)(long long);

/// An address-taken declaration gives the type's value as __kcfi_typeid_.
short t_b(short);
__attribute__((used)) static b_fn take_b = t_b;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_b"
// CHECK-NEXT: ".set __kcfi_typeid_t_b, [[#%u,B:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,B]]_foreign_get"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,B]]_foreign_get, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

struct g_ops {
  g_fn f;
};

struct a_ops {
  a_fn f;
  struct g_ops *next;
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
struct f_ops {
  f_fn f;
};
struct no_ops {
  int i;
};

__attribute__((dllimport)) void *imp_get(void);
void *foreign_get(void);
void *foreign_get2(void);
__attribute__((dllimport)) extern void *imp_data;
extern void *foreign_data;
void *mine(void) { return 0; }

void convert(void) {
  /// A call to a known import, implicitly and explicitly converted; a pointer
  /// in the record is not followed.
  struct a_ops *a = imp_get();
  struct a_ops *a2 = (struct a_ops *)imp_get();
  struct no_ops *n = imp_get();
  struct b_ops *b = foreign_get();
  struct c_ops *c = imp_data;
  void *local = foreign_get2();
  struct d_ops *d = local;
  struct e_ops *e = foreign_data;
  struct f_ops *f = mine();
  (void)a, (void)a2, (void)n, (void)b, (void)c, (void)d, (void)e, (void)f;
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#C]]}
