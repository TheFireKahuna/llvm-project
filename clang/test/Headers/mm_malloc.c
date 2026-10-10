// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple x86_64-linux-gnu -o - | FileCheck %s --check-prefixes=CHECK,POSIX
// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple x86_64-pc-windows-msvc -o - | FileCheck %s --check-prefix=MSVC
// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple x86_64-w64-windows-gnu -o - | FileCheck %s --check-prefix=MINGW
// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple x86_64-unknown-windows-itanium -o - | FileCheck %s --check-prefixes=CHECK,POSIX
// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple aarch64-unknown-windows-itanium -o - | FileCheck %s --check-prefixes=CHECK,POSIX
// RUN: %clang_cc1 -internal-isystem %S/Inputs/include %s -emit-llvm -O1 -triple x86_64-pc-windows-ntposix -o - | FileCheck %s --check-prefixes=CHECK,POSIX
#include <mm_malloc.h>

_Bool align_test(void) {
// CHECK-LABEL: @align_test(
// CHECK:    ret i1 true
     void *p = _mm_malloc(1024, 16);
    _Bool ret = ((__UINTPTR_TYPE__)p % 16) == 0;
    _mm_free(p);
    return ret;
}

// An alignment of 0 is rounded up to the pointer size, as other alignments
// smaller than it are, before it reaches the allocator.
void *align_zero(void) {
// POSIX-LABEL: @align_zero(
// POSIX:    call i32 @posix_memalign(ptr {{.*}}, i64 noundef 8, i64 noundef 1024)
// MSVC-LABEL: @align_zero(
// MSVC:    call ptr @_aligned_malloc(i64 noundef 1024, i64 noundef 8)
// MINGW-LABEL: @align_zero(
// MINGW:    call ptr @__mingw_aligned_malloc(i64 noundef 1024, i64 noundef 8)
  return _mm_malloc(1024, 0);
}
