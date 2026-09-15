// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-llvm -O1 -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -emit-llvm -O1 -o - %s | FileCheck %s --check-prefix=ARM
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-obj -O3 -mllvm -verify-machineinstrs -o %t.x64.o %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -emit-obj -O3 -mllvm -verify-machineinstrs -o %t.arm.o %s

void work(void **);

unsigned long long recover(void **buffer, unsigned long long value) {
  if (__builtin_experimental_nt_recovery(buffer))
    return value;
  work(buffer);
  return 0;
}

void ignored(void **buffer) {
  __builtin_experimental_nt_recovery(buffer);
  work(buffer);
}

// X64-LABEL: define{{.*}} @recover(
// X64: call ptr @llvm.frameaddress.p0(i32 0)
// X64: callbr void @llvm.experimental.nt.recovery(ptr
// X64: to label %{{.*}} [label %{{.*}}]
// X64: call void @work
// X64-NOT: call i32 @__builtin

// ARM-LABEL: define{{.*}} @recover(
// ARM: call ptr @llvm.frameaddress.p0(i32 0)
// ARM: callbr void @llvm.experimental.nt.recovery(ptr
// ARM: to label %{{.*}} [label %{{.*}}]
// ARM: call void @work
