// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A callee can store its own function pointers into an object that a call
/// passes a pointer to in a variadic argument, or converted to a parameter of
/// type void * or a pointer to a character type. The function pointers that
/// such an object holds, itself or in records it holds, but not those that
/// pointers in it reach, open dynamically for a known import and join the
/// __kcfi_inflow_ facts of a C declaration that is not one. A pointer to a
/// const object, a function pointer passed by value, a call to a library
/// builtin and a call to a function the unit defines give nothing.

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
int t_c(int);
long t_d(long);
__attribute__((used)) static c_fn take_c = t_c;
__attribute__((used)) static d_fn take_d = t_d;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_c, [[#%u,C:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_d, [[#%u,D:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,C]]_foreign_config"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,C]]_foreign_config, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,D]]_foreign_read"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,D]]_foreign_read, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

struct h_ops {
  h_fn f;
};
struct a_ops {
  a_fn f;
  struct h_ops *next;
};
struct b_ops {
  b_fn f;
};
struct c_ops {
  c_fn f;
  struct h_ops *next;
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

__attribute__((dllimport)) int imp_config(int op, ...);
__attribute__((dllimport)) void imp_read(void *buf);
__attribute__((dllimport)) void imp_cread(const void *buf);
__attribute__((dllimport)) void *memset(void *, int, size_t);
int foreign_config(int op, ...);
void foreign_read(char *buf);
void mine(void *buf) {}

void use(void) {
  struct a_ops a;
  g_fn g;
  struct b_ops b;
  struct c_ops c;
  struct d_ops d;
  struct e_ops e;
  const struct f_ops f = {0};
  imp_config(1, &a);
  imp_config(2, &g);
  imp_config(3, t_d);
  imp_config(4, &f);
  imp_read(&b);
  imp_cread(&e);
  memset(&e, 0, sizeof e);
  foreign_config(1, &c);
  foreign_read((char *)&d);
  mine(&e);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_g({{.*}} !kcfi_type ![[#G:]]
long long w_g(long long x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#G]], ![[#A]], ![[#B]]}
