// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=X64
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=A64

// setjmp passes the UCRT the frame that longjmp unwinds to.

#include <setjmp.h>

jmp_buf buf;

// X64-LABEL: define {{.*}}@jump(
// X64: call i32 @_setjmp(ptr @buf, ptr %{{.*}}) #[[RT:[0-9]+]]
// X64: call void @longjmp(ptr noundef @buf, i32 noundef 1) #[[NR:[0-9]+]]
// A64-LABEL: define {{.*}}@jump(
// A64: call i32 @_setjmpex(ptr @buf, ptr %{{.*}}) #[[RT:[0-9]+]]
// A64: call void @longjmp(ptr noundef @buf, i32 noundef 1) #[[NR:[0-9]+]]
int jump(void) {
  if (setjmp(buf))
    return 1;
  longjmp(buf, 1);
}

// X64-DAG: attributes #[[RT]] = { returns_twice }
// X64-DAG: attributes #[[NR]] = { noreturn }
// A64-DAG: attributes #[[RT]] = { returns_twice }
// A64-DAG: attributes #[[NR]] = { noreturn }
