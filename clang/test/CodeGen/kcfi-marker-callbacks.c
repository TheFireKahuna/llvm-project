// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// Code that may be foreign may call a function of ours that it is handed
/// with pointers to its own functions, so the types that the function's
/// parameters receive open as those of an exported definition's parameters
/// do: dynamically for a function handed to a known import, in a parameter or
/// held in an object passed by pointer, by value or untyped; in the
/// __kcfi_inflow_ facts of a C declaration that is not one; for a call
/// through a type that opens, dynamically when the object opens it and in
/// its __kcfi_tinflow_ facts otherwise; and for a function that an exported
/// definition hands back in its result or through an out-parameter,
/// dynamically, or a C definition, in its __kcfi_param_ facts. A type that
/// opens so and that the object calls through opens the types that our
/// functions handed to it receive in turn. A callback whose parameters
/// receive no function pointer gives nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);
typedef long long (*g_fn)(long long);
typedef unsigned (*h_fn)(unsigned);
typedef unsigned char (*i_fn)(unsigned char);
typedef unsigned short (*j_fn)(unsigned short);
typedef unsigned long (*k_fn)(unsigned long);
typedef signed char (*m_fn)(signed char);
typedef _Bool (*n_fn)(_Bool);

struct b_ops {
  b_fn f;
};
struct mn_ops {
  m_fn m;
  n_fn n;
  struct b_ops *next;
};

typedef void (*cb_a)(a_fn);
typedef void (*cb_b)(struct b_ops *);
typedef void (*cb_c)(c_fn);
typedef void (*cb_d)(d_fn);
typedef void (*cb_e)(e_fn);
typedef void (*cb_f)(f_fn);
typedef void (*cb_g)(g_fn);
typedef void (*cb_h)(h_fn);
typedef void (*cb_i)(i_fn);
typedef void (*cb_k)(k_fn);
typedef void (*cb_int)(int);
typedef void (*cb_mn)(struct mn_ops *);
typedef void (*hand_j)(j_fn);
typedef void (*inner)(hand_j);
typedef void (*cb_inner)(inner);

struct reg_b {
  cb_b cb;
};
struct reg_c {
  cb_c cb;
};
struct reg_e {
  cb_e cb;
};

__attribute__((dllimport)) void imp_fp(cb_a a, cb_int n);
__attribute__((dllimport)) void imp_ptr(const struct reg_b *r);
__attribute__((dllimport)) void imp_val(struct reg_c r);
__attribute__((dllimport)) void imp_untyped(void *p);
__attribute__((dllimport)) void imp_inner(cb_inner cb);
void foreign_fp(cb_d d);
void foreign_mn(cb_mn mn);

typedef void (*reg_f)(cb_f);
typedef void (*reg_i)(cb_i);

static void my_a(a_fn f) {}
static void my_b(struct b_ops *o) {}
static void my_c(c_fn f) {}
static void my_d(d_fn f) {}
static void my_e(e_fn f) {}
static void my_f(f_fn f) {}
static void my_i(i_fn f) {}
static void my_int(int x) {}
static void my_mn(struct mn_ops *o) {}
static void my_hand_j(j_fn f) {}
static void my_inner(inner in) { in(my_hand_j); }

__attribute__((used)) static void use(void *h, reg_i ri) {
  struct reg_b rb = {my_b};
  struct reg_c rc = {my_c};
  struct reg_e re = {my_e};
  imp_fp(my_a, my_int);
  imp_ptr(&rb);
  imp_val(rc);
  imp_untyped(&re);
  imp_inner(my_inner);
  foreign_fp(my_d);
  foreign_mn(my_mn);
  ((reg_f)h)(my_f);
  ri(my_i);
}

__attribute__((dllexport)) cb_g exp_result(void) { return 0; }
__attribute__((dllexport)) void exp_out(cb_h *out) {}
cb_k c_result(void) { return 0; }

/// Address-taken declarations give each type's value as __kcfi_typeid_.
long t_d(long);
unsigned char t_i(unsigned char);
unsigned long t_k(unsigned long);
signed char t_m(signed char);
_Bool t_n(_Bool);
__attribute__((used)) static d_fn take_d = t_d;
__attribute__((used)) static i_fn take_i = t_i;
__attribute__((used)) static k_fn take_k = t_k;
__attribute__((used)) static m_fn take_m = t_m;
__attribute__((used)) static n_fn take_n = t_n;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_d, [[#%u,D:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_i"
// CHECK-NEXT: ".set __kcfi_typeid_t_i, [[#%u,I:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_k"
// CHECK-NEXT: ".set __kcfi_typeid_t_k, [[#%u,K:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_m"
// CHECK-NEXT: ".set __kcfi_typeid_t_m, [[#%u,M:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_n"
// CHECK-NEXT: ".set __kcfi_typeid_t_n, [[#%u,N:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,D]]_foreign_fp"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,D]]_foreign_fp, {{[0-9]+}}"
/// The types that mn_ops holds are a node; the pointer it holds is not
/// followed.
// CHECK-NEXT: ".weak __kcfi_inflow_n[[NODE:[0-9a-f]+]]_foreign_mn"
// CHECK-NEXT: ".set __kcfi_inflow_n[[NODE]]_foreign_mn, 0"
// CHECK-NEXT: ".weak __kcfi_node_[[NODE]]_00000000[[#%.8x,N]]"
// CHECK-NEXT: ".set __kcfi_node_[[NODE]]_00000000[[#%.8x,N]], {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_node_[[NODE]]_00000000[[#%.8x,M]]"
// CHECK-NEXT: ".set __kcfi_node_[[NODE]]_00000000[[#%.8x,M]], {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_param_00000000[[#%.8x,K]]_c_result"
// CHECK-NEXT: ".set __kcfi_param_00000000[[#%.8x,K]]_c_result, {{[0-9]+}}"
/// The witness's own parameter fact.
// CHECK-NEXT: ".weak __kcfi_param_{{[0-9a-f]+}}_w_inner"
// CHECK-NEXT: ".set __kcfi_param_{{[0-9a-f]+}}_w_inner, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_tinflow_00000000[[#%.8x,I]]_{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_tinflow_00000000[[#%.8x,I]]_{{[0-9a-f]+}}, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_e({{.*}} !kcfi_type ![[#E:]]
float w_e(float x) { return x; }
// CHECK-DAG: define {{.*}} @w_f({{.*}} !kcfi_type ![[#F:]]
double w_f(double x) { return x; }
// CHECK-DAG: define {{.*}} @w_g({{.*}} !kcfi_type ![[#G:]]
long long w_g(long long x) { return x; }
// CHECK-DAG: define {{.*}} @w_h({{.*}} !kcfi_type ![[#H:]]
unsigned w_h(unsigned x) { return x; }
// CHECK-DAG: define {{.*}} @w_j({{.*}} !kcfi_type ![[#J:]]
unsigned short w_j(unsigned short x) { return x; }
// CHECK-DAG: define {{.*}} @w_inner({{.*}} !kcfi_type ![[#INNER:]]
void w_inner(hand_j h) {}
// CHECK-DAG: define {{.*}} @my_e({{.*}} !kcfi_type ![[#CBE:]]

/// reg_f opens by the cast, cb_e because imp_untyped may store into re, and
/// cb_h because a caller of exp_out hands it a pointer to one; D, I, K, M
/// and N are left to the linker.
// CHECK: !kcfi.dynamic = !{![[#REGF:]], ![[#A]], ![[#B]], ![[#C]], ![[#E]], ![[#CBE]], ![[#INNER]], ![[#G]], ![[#CBH:]], ![[#H]], ![[#F]], ![[#J]]}
