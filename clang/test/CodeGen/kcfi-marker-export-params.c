// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// The KCFI types that a caller can hand to a function, which an exported
/// definition opens dynamically and a C definition gives the linker as
/// __kcfi_param_ facts, are those of its function pointer parameters and of
/// the function pointers held in the objects that its parameters hold or
/// point to, records held by value included. Pointers in those objects are
/// not followed.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);
typedef long long (*g_fn)(long long);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
char t_a(char);
short t_b(short);
int t_c(int);
long t_d(long);
float t_e(float);
double t_f(double);
long long t_g(long long);
__attribute__((used)) static a_fn take_a = t_a;
__attribute__((used)) static b_fn take_b = t_b;
__attribute__((used)) static c_fn take_c = t_c;
__attribute__((used)) static d_fn take_d = t_d;
__attribute__((used)) static e_fn take_e = t_e;
__attribute__((used)) static f_fn take_f = t_f;
__attribute__((used)) static g_fn take_g = t_g;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_a"
// CHECK-NEXT: ".set __kcfi_typeid_t_a, [[#%u,A:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_b"
// CHECK-NEXT: ".set __kcfi_typeid_t_b, [[#%u,B:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_c, [[#%u,C:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_d, [[#%u,D:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_e"
// CHECK-NEXT: ".set __kcfi_typeid_t_e, [[#%u,E:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_f"
// CHECK-NEXT: ".set __kcfi_typeid_t_f, [[#%u,F:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_g"
// CHECK-NEXT: ".set __kcfi_typeid_t_g, [[#%u,G:]] /* {{.*}} */"

struct by_val {
  a_fn f;
};

struct far {
  e_fn f;
};

struct inner {
  c_fn f;
};

struct outer {
  struct inner in;
  struct far *p;
  d_fn arr[2];
};

struct g_holder {
  g_fn f;
};

/// A function pointer parameter; a record passed by value; a record passed by
/// pointer, with a record held by value, an array of function pointers and a
/// pointer that is not followed; a pointer to a function pointer.
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,B]]_def_fp"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,B]]_def_fp, [[#B]]"
void def_fp(b_fn f) {}
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,A]]_def_val"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,A]]_def_val, [[#A]]"
void def_val(struct by_val v) {}
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,C]]_def_ptr"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,C]]_def_ptr, [[#C]]"
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,D]]_def_ptr"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,D]]_def_ptr, [[#D]]"
void def_ptr(struct outer *o) {}
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,F]]_def_fpp"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,F]]_def_fpp, [[#F]]"
void def_fpp(f_fn *p) {}
/// A pointer to a pointer to a record gives nothing.
void def_pp(struct g_holder **pp) {}
// CHECK-NOT:  __kcfi_param_

/// Exported definitions open the same types dynamically.
__attribute__((dllexport)) void exp_val(struct by_val v) {}
__attribute__((dllexport)) void exp_ptr(struct outer *o) {}
__attribute__((dllexport)) void exp_pp(struct g_holder **pp) {}

// CHECK-DAG: declare !kcfi_type ![[#ANODE:]] {{.*}}@t_a(
// CHECK-DAG: declare !kcfi_type ![[#CNODE:]] {{.*}}@t_c(
// CHECK-DAG: declare !kcfi_type ![[#DNODE:]] {{.*}}@t_d(
// CHECK:     !kcfi.dynamic = !{![[#ANODE]], ![[#CNODE]], ![[#DNODE]]}
