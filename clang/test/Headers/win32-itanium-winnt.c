// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/winrt \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/winrt \
// RUN:     -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/winrt \
// RUN:     -std=c++17 -emit-llvm -o - -x c++ %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/um \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/winrt \
// RUN:     -std=c++17 -emit-llvm -o - -x c++ %s | FileCheck %s

// ntdef.h and winnt.h choose their declaration macros by _MSC_VER, which
// Windows Itanium does not define. The wrappers define them first for the
// attributes clang supports, and replace the ones winnt.h defines regardless.

#include <ntdef.h>
#include <winnt.h>

#ifdef __cplusplus
#include <wrl/def.h>
#ifdef _MSC_VER
#error "_MSC_VER must not leak out of wrl/def.h"
#endif
#define _Static_assert static_assert
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

_Static_assert(_Alignof(NTDEF_ALIGNED) == 16, "");

// CHECK: @selectany = weak_odr {{.*}}global i32 1, comdat
EXTERN_C DECLSPEC_SELECTANY int selectany = 1;

// CHECK-LABEL: define {{.*}}@twice(
// CHECK-NOT: call
// CHECK: ret i32
EXTERN_C int twice(int x) { return WinntTwice(x); }

// CHECK-LABEL: define {{.*}}@choose(
// CHECK: unreachable
EXTERN_C int choose(int x) {
  switch (x) {
  case 0:
    return 1;
    DEFAULT_UNREACHABLE;
  }
}

// CHECK-LABEL: define {{.*}}@nocf(
// CHECK: call void %{{.*}}() #[[NOCF:[0-9]+]]
EXTERN_C DECLSPEC_GUARDNOCF void nocf(void (*f)(void)) { f(); }

#ifdef __cplusplus
_Static_assert(TYPE_ALIGNMENT(NTDEF_ALIGNED) == 16, "");
struct DECLSPEC_UUID("00000000-0000-0000-c000-000000000046") Interface {};
const void *uuid = &__uuidof(Interface);
void no_throw() WIN_NOEXCEPT;
_Static_assert(noexcept(no_throw()), "");
_ENUM_FLAG_CONSTEXPR int constant() { return 1; }
_Static_assert(constant() == 1, "");
#endif

// CHECK-NOT: WinntTwice
// CHECK: attributes #[[NOCF]] = { {{.*}}"guard_nocf"
