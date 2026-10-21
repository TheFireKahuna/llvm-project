// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s | FileCheck %s

// Each x86 version of a multiversioned function records the features its
// resolver tests, so that a call whose caller has them resolves statically.
// A version chosen by the processor's identity records none.

__attribute__((target_clones("avx2", "arch=x86-64-v4", "arch=znver4",
                             "default")))
int clones(void) { return 0; }

__attribute__((target_clones("fsrm", "ermsb", "default"))) int strings(void) {
  return 3;
}

__attribute__((target("avx512f"))) int targets(void) { return 1; }
__attribute__((target("default"))) int targets(void) { return 2; }

int use(void) { return clones() + strings() + targets(); }

// CHECK: define {{.*}}i32 @clones.avx2.0() #[[AVX2:[0-9]+]]
// CHECK: define {{.*}}i32 @clones.arch_x86-64-v4.1() #[[V4:[0-9]+]]
// CHECK: define {{.*}}i32 @clones.arch_znver4.2() #[[ZNVER4:[0-9]+]]
// CHECK: define {{.*}}i32 @clones.default.3() #[[DEFAULT:[0-9]+]]
// CHECK: define {{.*}}i32 @strings.fsrm.0() #[[FSRM:[0-9]+]]
// CHECK: define {{.*}}i32 @strings.ermsb.1() #[[ERMSB:[0-9]+]]
// CHECK: define {{.*}}i32 @targets.avx512f() #[[AVX512F:[0-9]+]]
// CHECK: define {{.*}}i32 @targets() #[[DEFAULT2:[0-9]+]]

// CHECK: attributes #[[AVX2]] = {{.*}}"fmv-features"="avx2"
// CHECK: attributes #[[V4]] = {{.*}}"fmv-features"="x86-64-v4"
// CHECK-NOT: attributes #[[ZNVER4]] = {{.*}}"fmv-features"
// CHECK: attributes #[[DEFAULT]] = {{.*}}"fmv-features" {{.*}}
// CHECK: attributes #[[FSRM]] = {{.*}}"fmv-features"="fsrm" {{.*}}"target-features"="{{.*}}+fsrm
// CHECK: attributes #[[ERMSB]] = {{.*}}"fmv-features"="ermsb" {{.*}}"target-features"="{{.*}}+ermsb
// CHECK: attributes #[[AVX512F]] = {{.*}}"fmv-features"="avx512f"
