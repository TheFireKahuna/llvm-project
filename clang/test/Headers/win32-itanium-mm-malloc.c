// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - -x c++ %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -DMALLOC_H_ONLY -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=MALLOC

// _mm_malloc allocates with posix_memalign, whose memory free releases, even
// when the UCRT's malloc.h, which defines it as a macro for _aligned_malloc,
// comes first, and malloc.h alone provides it. stdlib.h declares
// aligned_alloc and posix_memalign.

#include <malloc.h>
#ifdef MALLOC_H_ONLY
// MALLOC-LABEL: define {{.*}}@allocate_only(
// MALLOC: call i32 @posix_memalign(
// MALLOC-NOT: _aligned_malloc
void allocate_only(void) { _mm_free(_mm_malloc(64, 64)); }
#else
#include <mm_malloc.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// CHECK-LABEL: define {{.*}}@allocate(
// CHECK: call i32 @posix_memalign(
// CHECK: call {{.*}}ptr @aligned_alloc(
void *allocate(void) {
  _mm_free(_mm_malloc(64, 64));
  return aligned_alloc(64, 64);
}

#ifdef __cplusplus
}
#endif
#endif

// CHECK-NOT: _aligned_malloc
// CHECK-NOT: _aligned_free
