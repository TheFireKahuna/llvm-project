// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A call through a pointer of a type that may reach a function without a
/// prefix of ours hands back that function's function pointers, held in
/// what it returns and in what it stores through its out-parameters, the
/// pointers to non-const pointers; pointers in those objects are not
/// followed, and a pointer parameter to a non-const record gives nothing,
/// unlike for a known import. When the object opens the called type dynamically, these
/// types open too, and so on while a call through one of them opens more.
/// For a called type the object does not open, it gives the linker these
/// types as __kcfi_tinflow_<type>_<called type>, or a node of the types a
/// record holds, to open when the linker opens the called type. A called
/// type that hands back no function pointer gives nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);

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
  d_fn d;
  e_fn e;
  struct c_ops *next;
};

typedef struct a_ops *(*get_a)(void);
typedef struct b_ops *(*get_b)(int);
typedef get_b (*get_get_b)(void);
typedef void (*out_c)(struct c_ops **);
typedef void (*out_f)(f_fn *);
typedef void (*in_c)(struct c_ops *);
typedef struct d_ops *(*get_d)(long);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
int t_c(int);
void t_out_c(struct c_ops **);
void t_out_f(f_fn *);
struct d_ops *t_get_d(long);
__attribute__((used)) static c_fn take_c = t_c;
__attribute__((used)) static out_c take_out_c = t_out_c;
__attribute__((used)) static out_f take_out_f = t_out_f;
__attribute__((used)) static get_d take_get_d = t_get_d;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_c, [[#%u,C:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_out_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_out_c, [[#%u,OUTC:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_out_f"
// CHECK-NEXT: ".set __kcfi_typeid_t_out_f, [[#%u,OUTF:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_get_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_get_d, [[#%u,GETD:]] /* {{.*}} */"
/// The declarations' own inflow facts follow the pointer in d_ops.
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,C]]_t_out_c"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,C]]_t_out_c, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_[[F:[0-9a-f]+]]_t_out_f"
// CHECK-NEXT: ".set __kcfi_inflow_[[F]]_t_out_f, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_inflow_n[[REACHED:[0-9a-f]+]]_t_get_d"
// CHECK-NEXT: ".set __kcfi_inflow_n[[REACHED]]_t_get_d, 0"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[REACHED]]_{{[0-9a-f]+}}, {{[0-9]+}}"
/// The called types' facts: the record that out_c stores a pointer to, the
/// function pointer that out_f stores, and the types d_ops holds; in_c,
/// whose parameter is a pointer to a record, gives nothing.
// CHECK-NEXT: ".weak __kcfi_tinflow_00000000[[#%.8x,C]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_tinflow_00000000[[#%.8x,C]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_tinflow_[[F]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_tinflow_[[F]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_tinflow_n[[HELD:[0-9a-f]+]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_tinflow_n[[HELD]]_{{[0-9a-f]+}}, 0"
// CHECK-NEXT: ".weak __kcfi_node_[[HELD]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[HELD]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[HELD]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_node_[[HELD]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

/// The conversions open get_a and get_get_b dynamically.
int call_a(void *h) {
  get_a g = (get_a)h;
  return g()->f(1);
}

int call_b(void *h) {
  get_get_b gg = (get_get_b)h;
  return gg()(1)->f(2);
}

__attribute__((used)) static int call_c(out_c o, get_d gd, c_fn c, out_f of,
                                        in_c i) {
  struct c_ops *ops;
  f_fn f;
  o(&ops);
  of(&f);
  i(ops);
  return ops->f(1) + gd(1)->d(2) + c(3) + f(4);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_get_a({{.*}} !kcfi_type ![[#GETA:]]
struct a_ops *w_get_a(void) { return 0; }
// CHECK-DAG: define {{.*}} @w_get_b({{.*}} !kcfi_type ![[#GETB:]]
struct b_ops *w_get_b(int x) { return 0; }
// CHECK-DAG: define {{.*}} @w_get_get_b({{.*}} !kcfi_type ![[#GETGETB:]]
get_b w_get_get_b(void) { return 0; }

// CHECK: !kcfi.dynamic = !{![[#GETA]], ![[#GETGETB]], ![[#A]], ![[#GETB]], ![[#B]]}
