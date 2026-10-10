// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A record that a value loaded from a variable is converted to a pointer to,
/// or copied into from it, belongs to whatever the variable holds. A variable
/// that only this translation unit stores into, a local variable, a parameter
/// or a variable with internal linkage, holds the values of the calls and
/// loads that its initializer and its assignments store, through other such
/// variables, so the function pointers the record holds open dynamically for
/// a known import and join the __kcfi_inflow_ facts of a C declaration that
/// is not one. A C variable that is not a known import gives them in its own
/// __kcfi_inflow_ facts. A parameter that nothing assigns, a value that is
/// not directly a call or a load, and a call to a function the unit defines
/// give nothing.

typedef __SIZE_TYPE__ size_t;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);
typedef long long (*g_fn)(long long);
typedef unsigned (*h_fn)(unsigned);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
long t_d(long);
float t_e(float);
double t_f(double);
__attribute__((used)) static d_fn take_d = t_d;
__attribute__((used)) static e_fn take_e = t_e;
__attribute__((used)) static f_fn take_f = t_f;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_d, [[#%u,D:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_e"
// CHECK-NEXT: ".set __kcfi_typeid_t_e, [[#%u,E:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_f"
// CHECK-NEXT: ".set __kcfi_typeid_t_f, [[#%u,F:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,D]]_foreign_get"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,D]]_foreign_get, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,F]]_foreign_get2"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,F]]_foreign_get2, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,E]]_foreign_data"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,E]]_foreign_data, {{[0-9]+}}"
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
struct f_ops {
  f_fn f;
};
struct g_ops {
  g_fn f;
};
struct h_ops {
  h_fn f;
};

void *memcpy(void *, const void *, size_t);
__attribute__((dllimport)) void *imp_get(void);
__attribute__((dllimport)) void *imp_get2(void);
__attribute__((dllimport)) void *imp_get3(void);
void *foreign_get(void);
void *foreign_get2(void);
extern void *foreign_data;
void *mine(void) { return 0; }

static void *cache;

void convert(void *param, void *assigned, int i) {
  /// An initializer, an assignment and a chain through another local.
  void *init = imp_get();
  struct a_ops *a = init;
  void *later;
  later = imp_get2();
  struct b_ops *b = later;
  void *first = foreign_get();
  void *second = first;
  struct d_ops *d = second;
  /// A parameter that is assigned, and a variable with internal linkage.
  assigned = imp_get3();
  struct c_ops *c = assigned;
  cache = imp_get3();
  struct h_ops *h = cache;
  /// A copy from a local, and a load of a C variable.
  void *src = foreign_get2();
  struct f_ops f;
  memcpy(&f, src, sizeof f);
  struct e_ops *e = foreign_data;
  /// Nothing assigns the parameter, and the other values are no call or
  /// load.
  struct g_ops *g = param;
  void *mixed = i ? mine() : param;
  struct g_ops *g2 = mixed;
  void *own = mine();
  struct g_ops *g3 = own;
  (void)a, (void)b, (void)c, (void)d, (void)e, (void)g, (void)g2, (void)g3,
      (void)h;
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_h({{.*}} !kcfi_type ![[#H:]]
unsigned w_h(unsigned x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]], ![[#C]], ![[#H]]}
