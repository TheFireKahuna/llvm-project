// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -std=c++20 -triple x86_64-pc-windows-ntposix -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,X64
// RUN: %clang_cc1 -std=c++20 -triple aarch64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,A64
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-windows-itanium -mdefault-visibility-export-mapping=explicit -emit-llvm -o - %s | FileCheck %s --check-prefix=ABSENT
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s --check-prefix=NOMAP

// A native thread-local variable is its image's TLS index plus an offset in
// that image's TLS template, which another image can know neither of. The
// defining image exports, in place of a variable marked as crossing the
// shared-library boundary, a record of the address of its _tls_index, the
// variable's offset and the offset of the byte guarding its initialization,
// and exports the init function. A marked declaration reads the record in the
// wrapper and does what the defining image's wrapper does.

#define API __attribute__((visibility("default")))

struct S {
  S();
  ~S();
  int v;
};
int f();

// Definitions. The variable is not exported, the record is. A variable with
// no initialization to run is guarded by a byte that is always set, and its
// init function is empty.
API thread_local int def_plain = 1;
API thread_local int def_dyn = f();
API thread_local S def_obj;
thread_local int def_unmarked = 2;
template <class T> struct API Holder {
  static thread_local T value;
};
template <class T> thread_local T Holder<T>::value = T(f());
template struct Holder<long>;

// CHECK-DAG: @def_plain = dso_local thread_local global i32 1
// CHECK-DAG: @def_dyn = dso_local thread_local global i32 0
// CHECK-DAG: @def_unmarked = dso_local thread_local global i32 2
// CHECK-DAG: @__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat
// CHECK-DAG: @"def_plain$tls" = dllexport constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @def_plain to i32), i32 ptrtoint (ptr @__tls_set to i32) }, align 8
// CHECK-DAG: @"def_dyn$tls" = dllexport constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @def_dyn to i32), i32 ptrtoint (ptr @__tls_guard to i32) }, align 8
// CHECK-DAG: @"def_obj$tls" = dllexport constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @def_obj to i32), i32 ptrtoint (ptr @__tls_guard to i32) }, align 8
// CHECK-DAG: @"_ZN6HolderIlE5valueE$tls" = weak_odr dllexport constant { ptr, i32, i32 } { ptr @_tls_index, i32 ptrtoint (ptr @_ZN6HolderIlE5valueE to i32), i32 ptrtoint (ptr @_ZGVN6HolderIlE5valueE to i32) }, comdat($_ZN6HolderIlE5valueE), align 8
// CHECK-DAG: @_ZTH7def_dyn = dso_local dllexport alias void (), ptr @__tls_init
// CHECK-DAG: @_ZTH7def_obj = dso_local dllexport alias void (), ptr @__tls_init
// CHECK-DAG: @_ZTHN6HolderIlE5valueE = weak_odr dso_local dllexport alias void (), ptr @__cxx_global_var_init{{.*}}
// ABSENT-NOT: @"def_unmarked$tls"

// Declarations. The variable is never named, so it is not in the module.
API extern thread_local int use_plain;
API extern constinit thread_local int use_const;
API extern thread_local int &use_ref;
extern thread_local int use_unmarked;

int use() { return use_plain + use_const + use_ref; }
int use_local() { return use_unmarked; }

// ABSENT-NOT: @use_plain =
// ABSENT-NOT: @use_const =
// CHECK-DAG: @"use_plain$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @"use_const$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @use_unmarked = external dso_local thread_local global i32

// CHECK-LABEL: define linkonce_odr hidden noundef ptr @_ZTW9use_plain()
// CHECK:       [[IDXP:%.*]] = load ptr, ptr @"use_plain$tls", align 8, !invariant.load
// CHECK-NEXT:  [[IDX:%.*]] = load i32, ptr [[IDXP]], align 4, !invariant.load
// CHECK-NEXT:  [[OFF:%.*]] = load i32, ptr getelementptr inbounds nuw (i8, ptr @"use_plain$tls", i64 8), align 8, !invariant.load
// X64-NEXT:    [[ARR:%.*]] = load ptr, ptr addrspace(256) inttoptr (i64 88 to ptr addrspace(256)), align 8
// A64-NEXT:    [[TEB:%.*]] = call i64 @llvm.read_register.i64(metadata [[X18:![0-9]+]])
// A64-NEXT:    [[TEBP:%.*]] = inttoptr i64 [[TEB]] to ptr
// A64-NEXT:    [[ARRP:%.*]] = getelementptr inbounds i8, ptr [[TEBP]], i64 88
// A64-NEXT:    [[ARR:%.*]] = load ptr, ptr [[ARRP]], align 8
// CHECK-NEXT:  [[IDX64:%.*]] = zext i32 [[IDX]] to i64
// CHECK-NEXT:  [[SLOT:%.*]] = getelementptr inbounds ptr, ptr [[ARR]], i64 [[IDX64]]
// CHECK-NEXT:  [[BLOCK:%.*]] = load ptr, ptr [[SLOT]], align 8
// CHECK-NEXT:  [[GOFF:%.*]] = load i32, ptr getelementptr inbounds nuw (i8, ptr @"use_plain$tls", i64 12), align 4, !invariant.load
// CHECK-NEXT:  [[GOFF64:%.*]] = zext i32 [[GOFF]] to i64
// CHECK-NEXT:  [[GUARDP:%.*]] = getelementptr inbounds i8, ptr [[BLOCK]], i64 [[GOFF64]]
// CHECK-NEXT:  [[GUARD:%.*]] = load i8, ptr [[GUARDP]], align 1
// CHECK-NEXT:  [[CLEAR:%.*]] = icmp eq i8 [[GUARD]], 0
// CHECK-NEXT:  br i1 [[CLEAR]], label %[[INIT:.*]], label %[[DONE:.*]], !prof
// CHECK:       [[INIT]]:
// CHECK-NEXT:  call void @_ZTH9use_plain()
// CHECK-NEXT:  br label %[[DONE]]
// CHECK:       [[DONE]]:
// CHECK-NEXT:  [[OFF64:%.*]] = zext i32 [[OFF]] to i64
// CHECK-NEXT:  [[ADDR:%.*]] = getelementptr inbounds i8, ptr [[BLOCK]], i64 [[OFF64]]
// CHECK-NEXT:  ret ptr [[ADDR]]

// A constinit declaration of a trivially destructible type has no guard to
// check.
// CHECK-LABEL: define linkonce_odr hidden noundef ptr @_ZTW9use_const()
// CHECK-NOT:   _ZTH
// CHECK:       ret ptr

// A reference is loaded from the variable the record locates.
// CHECK-LABEL: define linkonce_odr hidden noundef ptr @_ZTW7use_ref()
// CHECK:       call void @_ZTH7use_ref()
// CHECK:       [[REF:%.*]] = getelementptr inbounds i8, ptr {{.*}}
// CHECK-NEXT:  [[VAL:%.*]] = load ptr, ptr [[REF]], align 8
// CHECK-NEXT:  ret ptr [[VAL]]

// An unmarked declaration is this image's own, as before.
// CHECK-LABEL: define linkonce_odr hidden noundef ptr @_ZTW12use_unmarked()
// CHECK:       call {{.*}} @llvm.threadlocal.address.p0(ptr align 4 @use_unmarked)
// CHECK:       declare dllimport void @_ZTH9use_plain()
// CHECK:       declare extern_weak void @_ZTH12use_unmarked()
// CHECK:       define dllexport void @_ZTH9def_plain()
// CHECK-NEXT:  ret void

// A64: [[X18]] = !{!"x18"}

// Without the visibility mapping, nothing is imported or exported.
// NOMAP-NOT: $tls
// NOMAP-NOT: dllexport
// NOMAP-NOT: dllimport void @_ZTH
