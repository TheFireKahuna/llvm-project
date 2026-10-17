// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=AUTO %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=AUTO %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=NO-AUTO %s

// On Windows Itanium and NT-POSIX, -fauto-import, as on MinGW, leaves an
// external variable declaration that is not marked imported not dso_local, so
// that it is reached through its import pointer in case a DLL provides it.
// A native thread-local variable cannot be imported.

extern int var;
extern _Thread_local int tls_var;
int defined_var;

// AUTO-DAG:    @var = external global i32
// AUTO-DAG:    @tls_var = external dso_local thread_local global i32
// AUTO-DAG:    @defined_var = dso_local global i32 0
// NO-AUTO-DAG: @var = external dso_local global i32

int use(void) { return var + tls_var + defined_var; }
