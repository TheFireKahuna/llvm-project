// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fsanitize=kcfi \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fsanitize=kcfi \
// RUN:   -target-feature +avx512f -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fsanitize=kcfi \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,A64
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fsanitize=kcfi \
// RUN:   -target-feature +sve2 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefixes=CHECK,A64
// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=LINUX
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=MSVC

// Windows Itanium and NT-POSIX keep a multiversioned function a GlobalIFunc on
// both architectures, with the function's KCFI type, since its address is a
// stub of its own, and with no compatibility alias. The function alone has
// its visibility: its versions and resolver are hidden.

// CHECK-DAG: @narrow = weak_odr dso_local ifunc i32 (), ptr @narrow.resolver, !kcfi_type ![[#TYPE:]]
// CHECK-DAG: @wide = weak_odr dso_local ifunc i32 (), ptr @wide.resolver, !kcfi_type ![[#TYPE]]
// CHECK-NOT: .ifunc =
// LINUX: @narrow.ifunc = weak_odr alias i32 (), ptr @narrow
// MSVC-NOT: ifunc

// Every processor the targets support has their baseline, so a resolver
// tests only what lies above it, and an option within it ends the resolver:
// no option after it is reached, and a resolver with nothing to test
// initializes nothing. It names the version that ends it. The unit's own
// target features never fold a test: its resolver may be the one the program
// keeps.

#ifdef __x86_64__
#define NARROW "sse4.2"
#define WIDE "avx512f", "avx2"
#else
#define NARROW "dotprod"
#define WIDE "sve2", "dotprod"
#endif

__attribute__((target_clones(NARROW, "default"))) int narrow(void) { return 1; }
// X64: define hidden i32 @narrow.sse4.2.0()
// X64: define weak_odr hidden ptr @narrow.resolver() {{.*}}!resolver_fallback ![[#NARROW:]]
// X64-NEXT: resolver_entry:
// X64-NEXT:   ret ptr @narrow.sse4.2.0
// A64: define weak_odr hidden ptr @narrow.resolver() {{.*}}!resolver_fallback ![[#NARROW:]]
// A64-NEXT: resolver_entry:
// A64-NEXT:   ret ptr @narrow._Mdotprod

// LINUX: define weak_odr ptr @narrow.resolver()
// LINUX:   call void @__cpu_indicator_init()
// LINUX:   and i32 %{{.*}}, 256

__attribute__((target_clones(WIDE, "default"))) int wide(void) { return 0; }
// X64: define hidden i32 @wide.avx512f.0()
// X64: define weak_odr hidden ptr @wide.resolver() {{.*}}!resolver_fallback ![[#WIDE:]]
// X64:        call void @__cpu_indicator_init()
// X64:        and i32 %{{.*}}, 32768
// X64:      resolver_else:
// X64-NEXT:   ret ptr @wide.avx2.1
// X64-NOT:  @wide.default
// A64: define weak_odr hidden ptr @wide.resolver() {{.*}}!resolver_fallback ![[#WIDE:]]
// A64:        call void @__init_cpu_features_resolver()
// A64:      resolver_else:
// A64-NEXT:   ret ptr @wide._Mdotprod
// A64-NOT:  @wide.default

#ifdef __x86_64__
// An x86-64 level is tested like a feature, and a named processor by
// identity, which no baseline implies.
__attribute__((target_clones("arch=x86-64-v3", "arch=x86-64-v4", "default")))
int level(void) { return 2; }
// X64: define weak_odr hidden ptr @level.resolver()
// X64:        load i32, ptr getelementptr {{.*}}@__cpu_features2
// X64:      resolver_else:
// X64-NEXT:   ret ptr @level.arch_x86-64-v3.0

__attribute__((target_clones("arch=znver4", "default"))) int cpu(void) { return 3; }
// X64: define weak_odr hidden ptr @cpu.resolver()
// X64:        call void @__cpu_indicator_init()
// X64:        icmp eq i32 %{{.*}}, 29
// X64:      resolver_else:
// X64-NEXT:   ret ptr @cpu.default.1
#endif

int use(void) { return narrow() + wide(); }

// X64-DAG: ![[#NARROW]] = !{ptr @narrow.sse4.2.0}
// X64-DAG: ![[#WIDE]] = !{ptr @wide.avx2.1}
// A64-DAG: ![[#NARROW]] = !{ptr @narrow._Mdotprod}
// A64-DAG: ![[#WIDE]] = !{ptr @wide._Mdotprod}
