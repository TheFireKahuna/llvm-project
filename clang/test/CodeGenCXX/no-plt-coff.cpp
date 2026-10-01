// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-plt \
// RUN:   -fcxx-exceptions -fexceptions -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefixes=CHECK,ITANIUM %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fno-plt \
// RUN:   -fcxx-exceptions -fexceptions -emit-llvm -o - %s \
// RUN:   | FileCheck --check-prefixes=CHECK,NTPOSIX %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fno-plt -emit-llvm -o - %s \
// RUN:   -DNO_ITANIUM | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fcxx-exceptions \
// RUN:   -fexceptions -emit-llvm -o - %s | FileCheck --check-prefix=PLT %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-plt \
// RUN:   -fvisibility=hidden -emit-llvm -o - %s -DNO_ITANIUM \
// RUN:   | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-plt \
// RUN:   -fvisibility=hidden -fapply-global-visibility-to-externs \
// RUN:   -emit-llvm -o - %s -DNO_ITANIUM | FileCheck --check-prefix=EXTERNS %s

// Under -fno-plt on COFF a call to a function the translation unit does not
// define goes through the import table, as it goes through the GOT on ELF, so
// the declaration is dllimport, and so is a runtime function's. A hidden
// declaration, an extern_weak one, and a definition later in the translation
// unit are not imported, and neither is a variable. On Windows Itanium the
// functions that every image's startup code defines are not imported either.
// A global visibility applies to definitions only, unless it is applied to
// declarations too.

#define HIDDEN __attribute__((visibility("hidden")))

extern "C" {
void plain_fn();
HIDDEN void hidden_fn();
__attribute__((weak)) void weak_fn();
void later_fn();
extern int plain_var;
}

// CHECK-DAG: declare dllimport void @plain_fn()
// CHECK-DAG: declare hidden void @hidden_fn()
// CHECK-DAG: declare extern_weak void @weak_fn()
// CHECK-DAG: define {{dso_local|hidden}} void @later_fn()
// CHECK-DAG: @plain_var = external dso_local global i32

// PLT-NOT: dllimport {{.*}}@plain_fn

// EXTERNS: declare hidden void @plain_fn()

int use() {
  plain_fn();
  hidden_fn();
  if (weak_fn)
    weak_fn();
  later_fn();
  return plain_var;
}

extern "C" void later_fn() {}

#ifndef NO_ITANIUM
struct E {
  ~E();
};

E global_e;

void raise() { throw 1; }

// ITANIUM-DAG: declare dllimport ptr @__cxa_allocate_exception(i64)
// ITANIUM-DAG: declare dllimport void @__cxa_throw(ptr, ptr, ptr)
// ITANIUM-DAG: declare dso_local i32 @__cxa_atexit(ptr, ptr, ptr)
// NTPOSIX-DAG: declare dllimport ptr @__cxa_allocate_exception(i64)
// NTPOSIX-DAG: declare dllimport void @__cxa_throw(ptr, ptr, ptr)
// NTPOSIX-DAG: declare dllimport i32 @__cxa_atexit(ptr, ptr, ptr)
#endif
