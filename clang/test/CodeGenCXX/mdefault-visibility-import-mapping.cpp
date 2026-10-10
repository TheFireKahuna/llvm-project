// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -fno-function-type-prefix -fno-auto-import \
// RUN:   -emit-llvm -o - %s -mdefault-visibility-export-mapping=explicit \
// RUN:   | FileCheck --check-prefixes=CHECK,MAPPED %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -fno-function-type-prefix -fno-auto-import \
// RUN:   -emit-llvm -o - %s -mdefault-visibility-export-mapping=all \
// RUN:   | FileCheck --check-prefixes=CHECK,MAPPED %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix \
// RUN:   -fno-function-type-prefix -fno-auto-import \
// RUN:   -emit-llvm -o - %s -mdefault-visibility-export-mapping=explicit \
// RUN:   | FileCheck --check-prefixes=CHECK,MAPPED %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium \
// RUN:   -fno-function-type-prefix -fno-auto-import \
// RUN:   -emit-llvm -o - %s | FileCheck --check-prefixes=CHECK,UNMAPPED %s
// RUN: %clang_cc1 -triple powerpc64-ibm-aix -emit-llvm -o - %s \
// RUN:   -mdefault-visibility-export-mapping=explicit -Wno-ignored-attributes \
// RUN:   | FileCheck --check-prefix=AIX %s

// On COFF, the visibility mapping that exports a definition with an explicit
// default visibility imports a declaration with one: the mark says that the
// entity lives in a shared library. An implicit visibility says nothing about
// a declaration, and a hidden declaration, an extern_weak one, a native
// thread-local variable and a declaration marked dllexport are not imported.
// A definition later in the translation unit is not imported. Targets without
// an import table are unchanged.

// AIX-NOT: dllimport

#define DEFAULT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))

extern "C" {
DEFAULT void marked_fn();
void plain_fn();
HIDDEN void hidden_fn();
DEFAULT __attribute__((weak)) void weak_fn();
__attribute__((dllexport)) DEFAULT void exported_fn();
DEFAULT void later_fn();
DEFAULT extern int marked_var;
extern int plain_var;
DEFAULT extern __thread int tls_var;
}

// A class's explicit visibility applies to its members.
struct DEFAULT S {
  void method();
  static int static_member;
};

// MAPPED-DAG:   @marked_var = external dllimport global i32
// UNMAPPED-DAG: @marked_var = external dso_local global i32
// CHECK-DAG:    @plain_var = external dso_local global i32
// CHECK-DAG:    @tls_var = external dso_local thread_local global i32
// MAPPED-DAG:   @_ZN1S13static_memberE = external dllimport global i32
// UNMAPPED-DAG: @_ZN1S13static_memberE = external dso_local global i32

// MAPPED-DAG:   declare dllimport void @marked_fn()
// UNMAPPED-DAG: declare dso_local void @marked_fn()
// CHECK-DAG:    declare dso_local void @plain_fn()
// CHECK-DAG:    declare hidden void @hidden_fn()
// CHECK-DAG:    declare extern_weak void @weak_fn()
// CHECK-DAG:    declare dso_local void @exported_fn()
// MAPPED-DAG:   declare dllimport void @_ZN1S6methodEv(
// UNMAPPED-DAG: declare dso_local void @_ZN1S6methodEv(
// MAPPED-DAG:   define dso_local dllexport void @later_fn()
// UNMAPPED-DAG: define dso_local void @later_fn()

int use() {
  marked_fn();
  plain_fn();
  hidden_fn();
  if (weak_fn)
    weak_fn();
  exported_fn();
  later_fn();
  S().method();
  return marked_var + plain_var + tls_var + S::static_member;
}

extern "C" void later_fn() {}
