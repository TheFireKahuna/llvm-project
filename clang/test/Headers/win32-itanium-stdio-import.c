// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=IMPORT
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -emit-llvm -o - %s | FileCheck %s --check-prefix=IMPORT
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -D_NO_CRT_STDIO_INLINE -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=IMPORT
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -D_CRT_STDIO_INLINE=__inline -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=INLINE

// The UCRT's formatted I/O functions, which its stdio headers define inline,
// are declared imported from the compiler runtime, which defines them once
// for the program, unless the translation unit defines _CRT_STDIO_INLINE.

#include <stdio.h>

int (*address(void))(char *, const char *, ...) { return sprintf; }

int use(char *Buffer) { return sprintf(Buffer, "%d", 1); }

// IMPORT:     declare {{.*}}dllimport i32 @sprintf(ptr {{.*}}, ...)
// IMPORT-NOT: define {{.*}}@sprintf(

// INLINE:     define linkonce_odr {{.*}}i32 @sprintf(
