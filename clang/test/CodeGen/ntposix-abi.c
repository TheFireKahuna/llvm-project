// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s --check-prefix=ARM

typedef unsigned long long u64;
struct Pair { u64 first, second; };
struct Mixed { u64 integer; double real; };

// X64-LABEL: define{{.*}}{ i64, i64 } @c_pair(
// ARM-LABEL: define{{.*}} [2 x i64] @c_pair(
struct Pair c_pair(struct Pair value) { return value; }

// X64-LABEL: define{{.*}}{ i64, double } @c_mixed(
// ARM-LABEL: define{{.*}} [2 x i64] @c_mixed(
struct Mixed c_mixed(struct Mixed value) { return value; }

// X64-LABEL: define{{.*}}win64cc void @system_pair(
// X64-SAME: sret(%struct.Pair)
// ARM-LABEL: define{{.*}} [2 x i64] @system_pair(
__attribute__((ms_abi)) struct Pair system_pair(struct Pair value) { return value; }
