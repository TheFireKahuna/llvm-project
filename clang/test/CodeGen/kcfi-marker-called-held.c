// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A cast between two of our own function pointer types opens the target
/// type dynamically, as a C library's table of functions of mixed types
/// does. A call through it opens the function pointers held in what the call
/// returns, but a pointer parameter to a context opens neither those that
/// the context holds nor those of a database it points to.

typedef struct ctx ctx;
typedef struct db db;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);

struct db {
  a_fn a;
  b_fn b;
};
struct ctx {
  db *d;
  d_fn hook;
};
struct c_ops {
  c_fn f;
};

typedef void (*xfunc_fn)(ctx *, int);
typedef struct c_ops *(*get_c)(void);
typedef void (*any_fn)(void);
struct func_def {
  any_fn x;
};

__attribute__((used)) static void run(struct func_def *f, ctx *c) {
  ((xfunc_fn)f->x)(c, 1);
}

__attribute__((used)) static int table(void *h) {
  return ((get_c)h)()->f(1);
}

/// The witness's own parameter fact, for the hook its context holds.
// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_param_{{[0-9a-f]+}}_w_xfunc"
// CHECK-NEXT: ".set __kcfi_param_{{[0-9a-f]+}}_w_xfunc, {{[0-9]+}}"
// CHECK-NOT:  {{__kcfi_(inflow|param|tinflow)_}}
// CHECK-DAG:  define {{.*}} @w_xfunc({{.*}} !kcfi_type ![[#X:]]
void w_xfunc(ctx *c, int i) {}
// CHECK-DAG:  define {{.*}} @w_get_c({{.*}} !kcfi_type ![[#GETC:]]
struct c_ops *w_get_c(void) { return 0; }
// CHECK-DAG:  define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }

// CHECK:      !kcfi.dynamic = !{![[#X]], ![[#GETC]], ![[#C]]}
