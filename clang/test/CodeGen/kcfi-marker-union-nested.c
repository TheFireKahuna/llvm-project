// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A function pointer that a union's member holds in a struct, an anonymous
/// struct or an array may have been written as another member of the union,
/// so its type opens dynamically, as for a function pointer that is the
/// member itself. An array or a struct that a pointer in the union points to
/// opens nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);
typedef float (*e_fn)(float);

// CHECK-DAG: define {{.*}} @w_a({{.*}} !kcfi_type ![[#A:]]
char w_a(char x) { return x; }
// CHECK-DAG: define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }
// CHECK-DAG: define {{.*}} @w_c({{.*}} !kcfi_type ![[#C:]]
int w_c(int x) { return x; }
// CHECK-DAG: define {{.*}} @w_d({{.*}} !kcfi_type ![[#D:]]
long w_d(long x) { return x; }
// CHECK-DAG: define {{.*}} @w_e({{.*}} !kcfi_type ![[#E:]]
float w_e(float x) { return x; }

struct a_ops {
  a_fn f;
};
struct e_ops {
  e_fn f;
};

union nested {
  struct a_ops s;
  void *p;
};

union anonymous {
  struct {
    b_fn f;
  };
  long l;
};

union array {
  c_fn fs[2];
  void *p;
};

union array_of_structs {
  struct {
    d_fn f;
  } ss[2];
  long long l;
};

union pointers {
  struct e_ops *s;
  e_fn *fs;
  void *p;
};

int read(union nested *n, union anonymous *a, union array *r,
         union array_of_structs *rs, union pointers *p, int i) {
  c_fn c = r->fs[i];
  return n->s.f('a') + a->f(1) + c(1) + rs->ss[i].f(1) + p->s->f(1) +
         p->fs[i](1);
}

/// D, read as a callee through an array, after A, B and C in the order the
/// reads are emitted. E is not listed.
// CHECK: !kcfi.dynamic = !{![[#C]], ![[#A]], ![[#B]], ![[#D]]}
