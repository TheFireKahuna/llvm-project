// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

/// A function type whose record is local to a function keeps the check
/// identifier as its precise identifier, since another translation unit
/// cannot name the record.

typedef int (*cb)(int *);

int use(void *h) {
  struct loc { cb f; };
  typedef struct loc *(*get_loc)(int *);
  get_loc g = (get_loc)h;
  struct loc *l = g(0);
  return l->f(0);
}

/// The call through get_loc opens cb, which struct loc holds, although loc is
/// local: the local record keeps its check identifier as its precise one.
// CHECK-DAG: define {{.*}} @w({{.*}} !kcfi_type ![[#CB:]]
int w(int *p) { return *p; }

// CHECK: !kcfi.dynamic = {{.*}}![[#CB]]
