// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -pic-level 2 \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefix=IMPORT %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -pic-level 2 -emit-llvm \
// RUN:   -o - %s | FileCheck --check-prefix=IMPORT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -pic-level 2 \
// RUN:   -fdirect-access-external-data -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefix=DIRECT %s

// On Windows Itanium and NT-POSIX, as in ELF's position-independent code, an
// external variable declaration that is not marked imported is not
// dso_local, so that it is reached through its import pointer in case a DLL
// provides it, unless -fdirect-access-external-data says it is in the image.
// A thread-local one is reached through its record alike.

extern int var;
extern _Thread_local int tls_var;
int defined_var;

// IMPORT-DAG: @var = external global i32
// IMPORT-DAG: @tls_var = external thread_local global i32
// IMPORT-DAG: @defined_var = dso_local global i32 0
// DIRECT-DAG: @var = external dso_local global i32
// DIRECT-DAG: @tls_var = external dso_local thread_local global i32

int use(void) { return var + tls_var + defined_var; }
