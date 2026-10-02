// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A function pointer read from a member of a union that has a member of
/// another type may have been written as that member, so its type opens
/// dynamically. A union whose members all have the type, a write and a
/// member of a struct open nothing.

typedef char (*a_fn)(char);
typedef short (*b_fn)(short);
typedef int (*c_fn)(int);
typedef long (*d_fn)(long);

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

union mixed {
  a_fn f;
  void *p;
};

union mixed_load {
  float (*f)(float);
  long long l;
};

union mixed_write {
  b_fn f;
  void *p;
};

union same {
  c_fn f;
  c_fn g;
};

struct plain {
  d_fn f;
  void *p;
};

float (*load(union mixed_load *l))(float) { return l->f; }

int call(union mixed *m, union mixed_write *w, union same *s, struct plain *p) {
  w->f = w_b;
  return m->f('a') + s->f(1) + p->f(1);
}

/// A read as a value and a read as a callee. B, C and D are not listed.
// CHECK: !kcfi.dynamic = !{![[#E]], ![[#A]]}
