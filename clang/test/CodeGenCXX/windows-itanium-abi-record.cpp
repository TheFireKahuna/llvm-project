// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -fno-autolink \
// RUN:   -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm \
// RUN:   -fclang-abi-compat=23 -o - %s | FileCheck --check-prefix=NONE %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - \
// RUN:   -x c %s | FileCheck --check-prefix=NONE %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=NONE %s

// On Windows Itanium and NT-POSIX a C++ object records the revision of the
// C++ ABI it was compiled for, so that linking it with an object compiled for
// another revision fails. -fno-autolink keeps the record; a C object, and one
// compiled for Clang 23's ABI, which predates the record, carry none.

// CHECK: !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_CXX_ABI=1\22"}
// NONE-NOT: _WIN_ITANIUM_CXX_ABI

int f(void) { return 0; }
