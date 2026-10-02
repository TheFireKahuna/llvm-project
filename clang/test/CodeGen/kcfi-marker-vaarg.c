// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A variadic function receives function pointers in the variadic arguments
/// it reads with va_arg as it does in its parameters: a function pointer, and
/// the function pointers a record read by value or by pointer holds, without
/// following pointers in it. They join the types of an externally visible C
/// definition's __kcfi_param_ facts, or open dynamically for an exported
/// definition; a variadic function with internal linkage, which only this
/// unit calls, gives nothing. A function that reads a va_list it was handed
/// may read anyone's arguments, so the types it reads open dynamically.

typedef __builtin_va_list va_list;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
char t_a(char);
short t_b(short);
float t_e(float);
__attribute__((used)) static a_fn take_a = t_a;
__attribute__((used)) static b_fn take_b = t_b;
__attribute__((used)) static e_fn take_e = t_e;

struct other {
  f_fn f;
};
struct b_ops {
  b_fn f;
  struct other *o;
};
struct e_ops {
  e_fn f;
};

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_a"
// CHECK-NEXT: ".set __kcfi_typeid_t_a, [[#%u,A:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_b"
// CHECK-NEXT: ".set __kcfi_typeid_t_b, [[#%u,B:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_e"
// CHECK-NEXT: ".set __kcfi_typeid_t_e, [[#%u,E:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,A]]_def_va"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,A]]_def_va, [[#A]]"
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,B]]_def_va"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,B]]_def_va, [[#B]]"
// CHECK-NEXT: ".weak __kcfi_param_[[#%.8x,E]]_def_va"
// CHECK-NEXT: ".set __kcfi_param_[[#%.8x,E]]_def_va, [[#E]]"
// CHECK-NOT:  __kcfi_
int def_va(int op, ...) {
  va_list ap;
  __builtin_va_start(ap, op);
  a_fn a = __builtin_va_arg(ap, a_fn);
  struct b_ops *b = __builtin_va_arg(ap, struct b_ops *);
  struct e_ops e = __builtin_va_arg(ap, struct e_ops);
  __builtin_va_end(ap);
  return a(1) + b->f(2) + e.f(3);
}

__attribute__((dllexport)) int exp_va(int op, ...) {
  va_list ap;
  __builtin_va_start(ap, op);
  c_fn c = __builtin_va_arg(ap, c_fn);
  __builtin_va_end(ap);
  return c(op);
}

static int local_va(int op, ...) {
  va_list ap;
  __builtin_va_start(ap, op);
  f_fn f = __builtin_va_arg(ap, f_fn);
  __builtin_va_end(ap);
  return f(op);
}

int fwd(va_list ap) {
  d_fn d = __builtin_va_arg(ap, d_fn);
  return d(1) + local_va(1, d);
}

// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_d({{.*}} !kcfi_type ![[#D:]]
long w_d(long x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#D]], ![[#C]]}
