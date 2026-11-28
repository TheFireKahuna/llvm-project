// DEFINE: %{cc} = %clang_cc1 -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fms-extensions | FileCheck --check-prefixes=CHECK,AUTOLINK %s
// RUN: %{cc} -triple aarch64-unknown-windows-itanium -fms-extensions -fno-autolink | FileCheck --check-prefixes=CHECK,NOAUTOLINK %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -fms-extensions -fno-autolink | FileCheck --check-prefixes=CHECK,NOAUTOLINK %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fclang-abi-compat=23 | FileCheck --check-prefix=NONE %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -x c | FileCheck --check-prefix=NONE %s
// RUN: %{cc} -triple x86_64-pc-windows-msvc | FileCheck --check-prefix=NONE %s

// On Windows Itanium and NT-POSIX a C++ object records the revision of the
// C++ ABI it was compiled for, so that a link with an object compiled for
// another revision fails. A C object carries none, nor does one compiled for
// Clang 23's ABI, which predates the record. -fno-autolink keeps a mismatch
// record there, and drops only the names of libraries.

#pragma comment(lib, "foo")
#pragma detect_mismatch("key", "value")

// CHECK-DAG:      !{!"/FAILIFMISMATCH:\22key=value\22"}
// CHECK-DAG:      !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_CXX_ABI=1\22"}
// AUTOLINK-DAG:   !{!"/DEFAULTLIB:foo.lib"}
// NOAUTOLINK-NOT: DEFAULTLIB
// NONE-NOT:       _WIN_ITANIUM_CXX_ABI

int f(void) { return 0; }
