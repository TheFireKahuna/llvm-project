// Every function carries a type prefix by default on Windows Itanium and
// NT-POSIX on x86-64 and AArch64, whether or not KCFI checks calls, and on no
// other target, the SCEI flavour of Windows Itanium included.

// RUN: %clang_cc1 -E -triple x86_64-unknown-windows-itanium %s -o - \
// RUN:   | FileCheck %s --check-prefix=PREFIX
// RUN: %clang_cc1 -E -triple aarch64-unknown-windows-itanium %s -o - \
// RUN:   | FileCheck %s --check-prefix=PREFIX
// RUN: %clang_cc1 -E -triple x86_64-pc-windows-ntposix %s -o - \
// RUN:   | FileCheck %s --check-prefix=PREFIX
// RUN: %clang_cc1 -E -triple aarch64-pc-windows-ntposix %s -o - \
// RUN:   | FileCheck %s --check-prefix=PREFIX
// RUN: %clang_cc1 -E -triple x86_64-unknown-linux-gnu -ffunction-type-prefix \
// RUN:     %s -o - | FileCheck %s --check-prefix=PREFIX

// RUN: %clang_cc1 -E -triple x86_64-unknown-windows-itanium \
// RUN:     -fno-function-type-prefix %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %clang_cc1 -E -triple x86_64-pc-windows-msvc %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %clang_cc1 -E -triple i686-unknown-windows-itanium %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %clang_cc1 -E -triple thumbv7-unknown-windows-itanium %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %clang_cc1 -E -triple x86_64-scei-windows-itanium %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX
// RUN: %clang_cc1 -E -triple x86_64-unknown-linux-gnu -fsanitize=kcfi %s -o - \
// RUN:   | FileCheck %s --check-prefix=NO-PREFIX

// PREFIX: int FunctionTypePrefix();
// NO-PREFIX: int NoFunctionTypePrefix();

#if __has_feature(function_type_prefix)
int FunctionTypePrefix();
#else
int NoFunctionTypePrefix();
#endif
