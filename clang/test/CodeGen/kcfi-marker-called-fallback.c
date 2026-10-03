// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// A function type without a prototype may be any function of its check
/// identifier, so opening it opens every precise type of that check
/// identifier: its precise key is 1 << 32 | check, which prints with 1 in the
/// high word. A function type whose record is local to a function keeps the
/// check identifier as its precise identifier, since another translation unit
/// cannot name the record; its precise key prints with 0 in the high word.

typedef int (*noproto)();
typedef int (*cb)(int *);

// CHECK:      module asm
/// The unprototyped type opens as all precise types of its check identifier,
/// the key with 1 in the high word.
// CHECK:      ".weak __kcfi_popen_00000001{{[0-9a-f]+}}"
// CHECK-NEXT: ".set __kcfi_popen_00000001{{[0-9a-f]+}}, {{[0-9]+}}"

int use(void *h) {
  noproto np = (noproto)h;
  struct loc { cb f; };
  typedef struct loc *(*get_loc)(int *);
  get_loc g = (get_loc)h;
  struct loc *l = g(0);
  return np(1) + l->f(0);
}

/// The call through get_loc opens cb, which struct loc holds, although loc is
/// local: the local record keeps its check identifier as its precise one.
// CHECK-DAG: define {{.*}} @w({{.*}} !kcfi_type ![[#CB:]]
int w(int *p) { return *p; }

// CHECK: !kcfi.dynamic = {{.*}}![[#CB]]
