// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ffunction-type-prefix -emit-llvm -Wno-ignored-attributes -o - %s | FileCheck %s --check-prefix=ELF --implicit-check-not=kcfi.dynamic --implicit-check-not=__kcfi_inflow_ --implicit-check-not=__kcfi_param_

/// Variables hold function pointers that another image or foreign code reads
/// or writes. Imported data that the unit references opens the types
/// reachable from its type dynamically, and an exported variable the types it
/// holds or points to. A referenced C variable declaration that is not a known
/// import gives the linker its reachable types as __kcfi_inflow_ facts, and an
/// externally visible C variable definition the types it holds or points to as
/// __kcfi_param_ facts.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);
typedef double (*f_fn)(double);
typedef long long (*g_fn)(long long);
typedef unsigned char (*h_fn)(unsigned char);
typedef unsigned short (*i_fn)(unsigned short);
typedef unsigned (*j_fn)(unsigned);

/// Address-taken declarations give each type's value as __kcfi_typeid_.
long t_d(long);
double t_f(double);
long long t_g(long long);
__attribute__((used)) static d_fn take_d = t_d;
__attribute__((used)) static f_fn take_f = t_f;
__attribute__((used)) static g_fn take_g = t_g;

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_typeid_t_d"
// CHECK-NEXT: ".set __kcfi_typeid_t_d, [[#%u,D:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_f"
// CHECK-NEXT: ".set __kcfi_typeid_t_f, [[#%u,F:]] /* {{.*}} */"
// CHECK-NEXT: ".weak __kcfi_typeid_t_g"
// CHECK-NEXT: ".set __kcfi_typeid_t_g, [[#%u,G:]] /* {{.*}} */"

struct inner {
  c_fn f;
};

struct ops {
  b_fn f;
  struct inner *p;
};

struct g_holder {
  g_fn f;
};

struct far {
  j_fn f;
};

struct near {
  struct far *p;
};

/// Imported data: a function pointer, and a record whose pointers are
/// followed.
__attribute__((dllimport)) extern a_fn imp_cb;
__attribute__((dllimport)) extern struct ops imp_ops;
/// A declaration that only -fno-plt imports.
extern d_fn foreign_cb;
/// Declarations that are not referenced.
__attribute__((dllimport)) extern h_fn imp_unused;
extern e_fn foreign_unused;

/// Definitions: a function pointer, a pointer to a record and a record whose
/// pointer is not followed.
f_fn def_cb;
struct g_holder *def_holder;
struct near def_near;
static h_fn local_cb;
__attribute__((dllexport)) i_fn exp_cb;

int use(void) {
  local_cb = 0;
  return imp_cb('a') + imp_ops.f(1) + foreign_cb(1);
}

// CHECK-NEXT: ".weak __kcfi_inflow_00000000[[#%.8x,D]]_foreign_cb"
// CHECK-NEXT: ".set __kcfi_inflow_00000000[[#%.8x,D]]_foreign_cb, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_param_00000000[[#%.8x,F]]_def_cb"
// CHECK-NEXT: ".set __kcfi_param_00000000[[#%.8x,F]]_def_cb, {{[0-9]+}}"
// CHECK-NEXT: ".weak __kcfi_param_00000000[[#%.8x,G]]_def_holder"
// CHECK-NEXT: ".set __kcfi_param_00000000[[#%.8x,G]]_def_holder, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_i({{.*}} !kcfi_type ![[#I:]]
unsigned short w_i(unsigned short x) { return x; }

// CHECK: !kcfi.dynamic = !{![[#A]], ![[#B]], ![[#C]], ![[#I]]}

/// ELF targets record none of these facts.
// ELF: target triple
