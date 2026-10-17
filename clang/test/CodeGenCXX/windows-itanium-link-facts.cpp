// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefixes=CHECK,AUTOLINK %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fms-extensions \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -fno-autolink \
// RUN:   -o - %s | FileCheck --check-prefixes=CHECK,NOAUTOLINK %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -mdefault-visibility-export-mapping=explicit -emit-llvm -fno-autolink \
// RUN:   -o - %s | FileCheck --check-prefixes=CHECK,NOAUTOLINK %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions -emit-llvm \
// RUN:   -fno-autolink -o - %s | FileCheck --check-prefix=MSVC %s

// -fno-autolink drops the names of libraries to link, and on Windows Itanium
// and NT-POSIX only those: a mismatch record and the directives the object
// needs to be linked correctly stay, as MSVC's /Zl keeps all but default
// libraries. Elsewhere it drops every linker option, as before.

typedef decltype(sizeof(0)) size_t;
#pragma comment(lib, "foo")
#pragma detect_mismatch("key", "value")
void *operator new(size_t n) { return reinterpret_cast<void *>(n); }

// AUTOLINK-DAG:   !{!"/DEFAULTLIB:foo.lib"}
// NOAUTOLINK-NOT: DEFAULTLIB
// CHECK-DAG:      !{!"/FAILIFMISMATCH:\22key=value\22"}
// CHECK-DAG:      !{!"/OVERRIDABLE:_Znw{{[ym]}}"}
// CHECK-DAG:      !{!"/INCLUDE:__llvm_overrides_v1"}

// MSVC-NOT: llvm.linker.options
