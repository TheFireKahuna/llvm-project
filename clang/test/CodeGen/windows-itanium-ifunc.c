// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm \
// RUN:   -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm \
// RUN:   -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -O2 -emit-llvm \
// RUN:   -fsanitize=kcfi -o - %s | FileCheck %s --check-prefix=A64
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -O2 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=MSVC
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=FB
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -O2 -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefix=FBA

// Windows Itanium and NT-POSIX keep an ifunc to the backend, which lowers it
// to a stub that jumps through its import pointer, on both architectures. The
// stub carries the function's KCFI type. A multiversioned function's ifunc
// takes no compatibility alias.

static int impl(int x) { return x + 1; }
static int (*resolve(void))(int) { return impl; }
int f(int) __attribute__((ifunc("resolve")));

int (*address(void))(int) { return f; }

// CHECK: @f = dso_local ifunc i32 (i32), ptr @resolve, !kcfi_type ![[#TYPE:]]
// CHECK-NOT: .ifunc =
// CHECK: !{i32 [[#]]}

// X64-NOT: @f.ifunc

// A64: @f = dso_local ifunc i32 (i32), ptr @resolve, !kcfi_type
// MSVC-NOT: {{^@.*}} ifunc

// A multiversioned function's resolver names the version it falls back to,
// which the import pointer holds until the start-up writes it: the default, or
// an option within the target's baseline, which ends the resolver. A raw
// ifunc's resolver names none.
#ifdef __x86_64__
__attribute__((target_clones("avx512f", "default"))) int wide(void) { return 0; }
__attribute__((target_clones("avx512f", "avx2", "default"))) int folded(void) {
  return 1;
}
#else
__attribute__((target_clones("sve2", "default"))) int wide(void) { return 0; }
#endif
int use(void) { return wide(); }

// FB-NOT: define {{.*}} @resolve({{.*}}!resolver_fallback
// FB: define weak_odr hidden ptr @wide.resolver() {{.*}}!resolver_fallback ![[#WIDE:]]
// FB: define weak_odr hidden ptr @folded.resolver() {{.*}}!resolver_fallback ![[#FOLDED:]]
// FB: ![[#WIDE]] = !{ptr @wide.default.1}
// FB: ![[#FOLDED]] = !{ptr @folded.avx2.1}
// FBA: define weak_odr hidden ptr @wide.resolver() {{.*}}!resolver_fallback ![[#WIDE:]]
// FBA: ![[#WIDE]] = !{ptr @wide.default}
