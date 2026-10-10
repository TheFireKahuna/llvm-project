// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c99 -fsyntax-only -verify=hidden %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c11 -fsyntax-only -verify=visible %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=c99 -D_POSIX_C_SOURCE=200112L -DPOSIX_MEMALIGN \
// RUN:     -fsyntax-only -verify=hidden %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -std=gnu99 -DPOSIX_MEMALIGN -fsyntax-only -verify=visible %s
// visible-no-diagnostics

// As with glibc, a strict ISO C mode sees aligned_alloc only from C11 on and
// posix_memalign only for POSIX, so that a program may use the names itself.

#include <stdlib.h>

#ifdef POSIX_MEMALIGN
int (*posix_memalign_pointer)(void **, size_t, size_t) = posix_memalign;
#else
int posix_memalign;
#endif

void *allocate(void) {
  // hidden-error@+2 {{call to undeclared library function 'aligned_alloc'}}
  // hidden-note@+1 {{explicitly provide a declaration for 'aligned_alloc'}}
  return aligned_alloc(16, 16);
}
