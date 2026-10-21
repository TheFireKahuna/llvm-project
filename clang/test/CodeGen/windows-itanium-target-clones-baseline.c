// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=FLOOR
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=FLOOR
// RUN: %clang_cc1 -triple x86_64-linux-gnu -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=OTHER

// Every x86-64 processor Windows Itanium and NT-POSIX support has the
// x86-64-v3 level and the features of their baseline, so a resolver tests
// only what lies above it, and an option within it ends the resolver. The
// unit's own target features never fold a test: its resolver may be the one
// the program keeps.

__attribute__((target_clones("sse4.2", "default"))) int narrow(void) {
  return 1;
}
// FLOOR-LABEL: define weak_odr dso_local i32 @narrow()
// FLOOR-NEXT:  resolver_entry:
// FLOOR-NEXT:    musttail call i32 @narrow.sse4.2.0()

// OTHER-LABEL: define weak_odr ptr @narrow.resolver()
// OTHER:         call void @__cpu_indicator_init()
// OTHER:         and i32 %{{.*}}, 256

__attribute__((target_clones("avx2", "avx512f", "default"))) int wide(void) {
  return 0;
}
// FLOOR-LABEL: define weak_odr dso_local i32 @wide()
// FLOOR:         call void @__cpu_indicator_init()
// FLOOR:         and i32 %{{.*}}, 32768
// FLOOR:       resolver_else:
// FLOOR-NEXT:    musttail call i32 @wide.avx2.0()
// FLOOR-NOT:   @wide.default

__attribute__((target_clones("arch=x86-64-v3", "arch=x86-64-v4", "default")))
int level(void) {
  return 2;
}
// FLOOR-LABEL: define weak_odr dso_local i32 @level()
// FLOOR:         load i32, ptr getelementptr {{.*}}@__cpu_features2
// FLOOR:       resolver_else:
// FLOOR-NEXT:    musttail call i32 @level.arch_x86-64-v3.0()

// A named processor is tested by identity, which no baseline implies.
__attribute__((target_clones("arch=znver4", "default"))) int cpu(void) {
  return 3;
}
// FLOOR-LABEL: define weak_odr dso_local i32 @cpu()
// FLOOR:         call void @__cpu_indicator_init()
// FLOOR:         icmp eq i32 %{{.*}}, 29

int use(void) { return narrow() + wide() + level() + cpu(); }
