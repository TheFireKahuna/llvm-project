// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s

// A C thread-local that crosses the shared-library boundary is reached as a
// C++ one is: the defining image exports a record of its _tls_index, the
// variable's offset and the offset of a guard byte that is always set, never
// the variable, and an empty init function for an importer in C++ to name. A
// marked declaration reads the record, with no guard to test, and is left out
// of the module. An unmarked one stays a native thread-local declaration.

#define API __attribute__((visibility("default")))

API _Thread_local int defined = 3;
API extern _Thread_local int imported;
extern _Thread_local int unmarked;
_Thread_local int local;

int get(void) { return defined + imported + unmarked + local; }

// CHECK-DAG: @defined = dso_local thread_local global i32 3
// CHECK-DAG: @"defined$tls" = dllexport constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @defined to i32), i32 ptrtoint (ptr @__tls_set to i32) }
// CHECK-DAG: @__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat
// CHECK-DAG: @"imported$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @unmarked = external {{.*}}thread_local global i32
// CHECK-NOT: @imported =
// CHECK-NOT: @"local$tls"

// CHECK-LABEL: define {{.*}} @_ZTW8imported()
// CHECK-NOT:   call {{.*}}@_ZTH
// CHECK:       ret ptr

// CHECK: define dllexport void @_ZTH7defined()
// CHECK-NEXT: ret void
