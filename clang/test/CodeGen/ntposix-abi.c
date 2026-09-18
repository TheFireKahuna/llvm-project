// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=ARM

// Plain C is the platform convention: MS x64 on x86-64, AAPCS64 on AArch64.
// The SysV convention is available per declaration on x86-64.

typedef unsigned long long u64;
struct Pair { u64 first, second; };
struct Mixed { u64 integer; double real; };

// X64-LABEL: define{{.*}} void @c_pair(ptr {{.*}}sret(%struct.Pair){{.*}}, ptr {{.*}}%value)
// ARM-LABEL: define{{.*}} [2 x i64] @c_pair(
struct Pair c_pair(struct Pair value) { return value; }

// X64-LABEL: define{{.*}} void @c_mixed(ptr {{.*}}sret(%struct.Mixed){{.*}}, ptr {{.*}}%value)
// ARM-LABEL: define{{.*}} [2 x i64] @c_mixed(
struct Mixed c_mixed(struct Mixed value) { return value; }

// X64-LABEL: define{{.*}} void @system_pair(ptr {{.*}}sret(%struct.Pair){{.*}}, ptr {{.*}}%value)
// ARM-LABEL: define{{.*}} [2 x i64] @system_pair(
__attribute__((ms_abi)) struct Pair system_pair(struct Pair value) { return value; }

#ifdef __x86_64__
// X64-LABEL: define{{.*}} x86_64_sysvcc { i64, i64 } @sysv_pair(i64 %value.coerce0, i64 %value.coerce1)
__attribute__((sysv_abi)) struct Pair sysv_pair(struct Pair value) { return value; }
#endif
