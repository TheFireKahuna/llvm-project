// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-kcfi-marker -emit-llvm -o - %s | FileCheck %s

/// The result of a call to an allocation function is fresh memory, which
/// holds no function pointer, so a conversion of it to a pointer to a record
/// opens nothing: a function declared __declspec(restrict) as the UCRT's
/// allocators are, __attribute__((malloc)) or alloc_size, and the C library's
/// malloc, calloc and realloc, which carry alloc_size implicitly.

typedef __SIZE_TYPE__ size_t;
typedef char (*a_fn)(char);
typedef short (*b_fn)(short);

struct a_ops {
  a_fn f;
};
struct b_ops {
  b_fn f;
};

__declspec(dllimport) __declspec(restrict) void *imp_restrict(size_t);
__declspec(dllimport) __attribute__((malloc)) void *imp_malloc_attr(size_t);
__declspec(dllimport) __attribute__((alloc_size(1))) void *imp_alloc_size(size_t);
__declspec(dllimport) void *malloc(size_t);
__declspec(dllimport) void *calloc(size_t, size_t);
__declspec(dllimport) void *realloc(void *, size_t);
__declspec(restrict) void *foreign_alloc(size_t);
__declspec(dllimport) void *imp_get(void);

void convert(void *old) {
  struct a_ops *a1 = imp_restrict(sizeof *a1);
  struct a_ops *a2 = imp_malloc_attr(sizeof *a2);
  struct a_ops *a3 = imp_alloc_size(sizeof *a3);
  struct a_ops *a4 = malloc(sizeof *a4);
  struct a_ops *a5 = calloc(1, sizeof *a5);
  struct a_ops *a6 = realloc(old, sizeof *a6);
  struct a_ops *a7 = foreign_alloc(sizeof *a7);
  /// Another known import's result opens the record's types.
  struct b_ops *b = imp_get();
  (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)a6, (void)a7;
  (void)b;
}

// CHECK-NOT:  module asm
// CHECK-DAG:  define {{.*}} @w_b({{.*}} !kcfi_type ![[#B:]]
short w_b(short x) { return x; }

// CHECK:      !kcfi.dynamic = !{![[#B]]}
