// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A function of ours passed itself in a variadic argument, or converted to
/// an untyped pointer parameter, may be called by the callee with pointers to
/// the callee's own functions, as one passed in a parameter of its type may:
/// the types that its parameters receive open dynamically for a known import
/// and join the __kcfi_inflow_ facts of a C declaration that is not one.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
int t_c(int);
__attribute__((used)) static c_fn take_c = t_c;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_c"
// CHECK-NEXT: ".set __kcfi_typeid_t_c, [[#%u,C:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,C]]_foreign_printf"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,C]]_foreign_printf, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

__attribute__((dllimport)) int imp_printf(const char *fmt, ...);
__attribute__((dllimport)) void imp_untyped(void *p);
int foreign_printf(const char *fmt, ...);

static void my_a(a_fn f) {}
static void my_b(b_fn f) {}
static void my_c(c_fn f) {}

__attribute__((used)) static void use(void) {
  imp_printf("%p", my_a);
  imp_untyped((void *)my_b);
  foreign_printf("%p", my_c);
}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]]}
