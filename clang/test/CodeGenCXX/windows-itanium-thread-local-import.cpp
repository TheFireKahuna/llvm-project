// On Windows Itanium and NT-POSIX a thread-local variable another image may
// define, one the unit only declares, is reached through its record
// <name>$tls: the address of the defining image's TLS index, the variable's
// offset and its guard's. The module declares the record and never the
// variable. A dllimport or explicit default visibility declaration is another
// image's; any other is of whichever image the link finds the variable in.

// DEFINE: %{cc} = %clang_cc1 -fdeclspec -pic-level 2 -emit-llvm -o - %s \
// DEFINE:   -mdefault-visibility-export-mapping=explicit
// DEFINE: %{check} = FileCheck %s \
// DEFINE:   --implicit-check-not='@{{(dyn|obj|ref|constant|imported|marked|unmarked)}} ='
// RUN: %{cc} -std=c++20 -triple x86_64-unknown-windows-itanium \
// RUN:   | %{check} --check-prefixes=CHECK,CXX
// RUN: %{cc} -std=c++20 -triple aarch64-unknown-windows-ntposix \
// RUN:   | FileCheck %s --check-prefix=OWNED --implicit-check-not='$tls' \
// RUN:     --implicit-check-not=@llvm.thread.pointer
// RUN: %{cc} -x c -triple x86_64-unknown-windows-itanium | %{check}

// The option that reaches a declared variable directly leaves an unmarked
// declaration to this image, as does a visibility that is not default.
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-windows-itanium \
// RUN:   -fdeclspec -fdirect-access-external-data -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=DIRECT

// No other target has records.
// RUN: %{cc} -std=c++20 -triple x86_64-w64-windows-gnu -DNO_ATTRIBUTE \
// RUN:   | FileCheck %s --check-prefix=NONE --implicit-check-not='$tls'

#define MARK __attribute__((visibility("default")))
#ifdef NO_ATTRIBUTE
#define IMPORT
#else
#define IMPORT __declspec(dllimport)
#endif

#ifdef __cplusplus
struct S {
  S();
  ~S();
  int v;
};
MARK extern thread_local int dyn;
IMPORT extern thread_local S obj;
MARK extern thread_local int &ref;
MARK extern constinit thread_local int constant;
extern thread_local int unmarked;
__attribute__((visibility("hidden"))) extern thread_local int hidden;
extern "C" {
#endif
IMPORT extern __thread int imported;
MARK extern __thread int marked;
extern __thread int plain;
#ifdef __cplusplus
}
#endif

// CXX-DAG:   @"dyn$tls" = external dllimport constant { ptr, i32, i32 }
// CXX-DAG:   @"obj$tls" = external dllimport constant { ptr, i32, i32 }
// CXX-DAG:   @"ref$tls" = external dllimport constant { ptr, i32, i32 }
// CXX-DAG:   @"constant$tls" = external dllimport constant { ptr, i32, i32 }
// CXX-DAG:   @"unmarked$tls" = external dllimport constant { ptr, i32, i32 }
// CXX-DAG:   @hidden = external hidden thread_local global i32
// CHECK-DAG: @"imported$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @"marked$tls" = external dllimport constant { ptr, i32, i32 }
// CHECK-DAG: @plain = external thread_local global i32
// DIRECT-DAG: @unmarked = external dso_local thread_local global i32
// DIRECT-DAG: @plain = external dso_local thread_local global i32
// Under NT-POSIX's owned model the unit declares the variable, and a guard
// that names it, and the back end reaches both through the record of their
// image's place word.
// OWNED-DAG: @obj = external thread_local global %struct.S
// OWNED-DAG: @"dyn$guard" = external thread_local global i8, !tls_record ![[DYN:[0-9]+]]
// OWNED-DAG: @imported = external thread_local global i32
// OWNED-DAG: ![[DYN]] = !{ptr @dyn}
// NONE-DAG:  @dyn = external
// NONE-DAG:  @marked = external

// A variable that needs no initialization in a thread, as none of C does, is
// the record's block and offset. The record never changes, and neither does
// the index, which is four bytes wherever its address is, or the block.
// CHECK-LABEL: define {{.*}} @{{.*}}use_marked
// CHECK:      [[P:%.*]] = load ptr, ptr @"marked$tls", align 8, !invariant.load [[E:![0-9]+]], !dereferenceable [[FOUR:![0-9]+]], !align [[FOUR]], !noundef [[E]]
// CHECK-NEXT: [[I:%.*]] = load i32, ptr [[P]], align 4, !invariant.load [[E]]
// CHECK-NEXT: [[A:%.*]] = call ptr @llvm.thread.pointer.p0()
// CHECK-NEXT: [[X:%.*]] = zext i32 [[I]] to i64
// CHECK-NEXT: [[S:%.*]] = getelementptr inbounds ptr, ptr [[A]], i64 [[X]]
// CHECK-NEXT: [[B:%.*]] = load ptr, ptr [[S]], align 8, !invariant.load [[E]]{{$}}
// CHECK-NEXT: [[O:%.*]] = load i32, ptr getelementptr inbounds nuw (i8, ptr @"marked$tls", i64 8), align 4, !invariant.load [[E]]
// CHECK-NEXT: [[Z:%.*]] = zext i32 [[O]] to i64
// CHECK-NEXT: [[V:%.*]] = getelementptr inbounds i8, ptr [[B]], i64 [[Z]]
// CHECK-NEXT: ret ptr [[V]]
int *use_marked(void) { return &marked; }

// CHECK-LABEL: define {{.*}} @{{.*}}use_imported
// CHECK:       load ptr, ptr @"imported$tls"
// CHECK-NOT:   call void
int use_imported(void) { return imported; }

// An unmarked declaration of a variable that no thread initializes stays a
// variable that is not DSO local: the back end reads its record, and a link
// that proves it local before code is generated needs none.
// CHECK-LABEL: define {{.*}} @{{.*}}use_plain
// CHECK:       call {{.*}}ptr @llvm.threadlocal.address.p0(ptr {{.*}}@plain)
// CHECK-NOT:   call void
int use_plain(void) { return plain; }

#ifdef __cplusplus
// CXX-LABEL: define {{.*}} @_Z12use_constantv
// CXX:       load ptr, ptr @"constant$tls"
// CXX-NOT:   call {{.*}} @_ZTW
int use_constant() { return constant; }

// CXX-LABEL: define {{.*}} @_Z7use_dynv
// CXX:       call ptr @_ZTW3dyn()
int use_dyn() { return dyn; }

// Any other variable goes through its wrapper, which tests the guard byte
// that the record places and calls the initialization function that the
// other image exports with the record, real or empty.
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW3dyn() {{.*}} comdat {
// CXX:       [[B:%.*]] = load ptr, ptr %{{.*}}, align 8
// CXX-NEXT:  [[GO:%.*]] = load i32, ptr getelementptr inbounds nuw (i8, ptr @"dyn$tls", i64 12), align 4, !invariant.load
// CXX-NEXT:  [[GZ:%.*]] = zext i32 [[GO]] to i64
// CXX-NEXT:  [[G:%.*]] = getelementptr inbounds i8, ptr [[B]], i64 [[GZ]]
// CXX-NEXT:  [[O:%.*]] = load i32, ptr getelementptr inbounds nuw (i8, ptr @"dyn$tls", i64 8), align 4, !invariant.load
// CXX-NEXT:  [[Z:%.*]] = zext i32 [[O]] to i64
// CXX-NEXT:  [[V:%.*]] = getelementptr inbounds i8, ptr [[B]], i64 [[Z]]
// CXX-NEXT:  [[BYTE:%.*]] = load i8, ptr [[G]], align 1
// CXX-NEXT:  [[UNSET:%.*]] = icmp eq i8 [[BYTE]], 0
// CXX-NEXT:  br i1 [[UNSET]], label %[[INIT:.*]], label %[[DONE:.*]], !prof [[UNLIKELY:![0-9]+]]
// CXX:       [[INIT]]:
// CXX-NEXT:  call void @_ZTH3dyn()
// CXX-NEXT:  br label %[[DONE]]
// CXX:       [[DONE]]:
// CXX-NEXT:  ret ptr [[V]]

// CXX-LABEL: define {{.*}} @_Z7use_objv
// CXX:       call ptr @_ZTW3obj()
int use_obj() { return obj.v; }
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW3obj()
// CXX:       call void @_ZTH3obj()

// The wrapper of a reference returns what the reference names.
// CXX-LABEL: define {{.*}} @_Z7use_refv
// CXX:       call ptr @_ZTW3ref()
int use_ref() { return ref; }
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW3ref()
// CXX:       call void @_ZTH3ref()
// CXX:       [[R:%.*]] = load ptr, ptr %{{.*}}, align 8
// CXX-NEXT:  ret ptr [[R]]

// The definition of an unmarked declaration, wherever the link finds it, may
// have no initialization function, so the reference to it is weak. Its guard
// is then never unset, and the wrapper is that of a known import.
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW8unmarked()
// CXX:       load i32, ptr getelementptr inbounds nuw (i8, ptr @"unmarked$tls", i64 12)
// CXX:       br i1 %{{.*}}, label %[[INIT:.*]], label %[[DONE:.*]], !prof [[UNLIKELY]]
// CXX:       [[INIT]]:
// CXX-NEXT:  call void @_ZTH8unmarked()
// CXX-NEXT:  br label %[[DONE]]
// DIRECT-LABEL: define linkonce_odr hidden noundef ptr @_ZTW8unmarked()
// DIRECT:       icmp ne ptr @_ZTH8unmarked, null
// DIRECT:       call align 4 ptr @llvm.threadlocal.address.p0(ptr align 4 @unmarked)
int use_unmarked() { return unmarked; }

// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW6hidden()
// CXX:       call align 4 ptr @llvm.threadlocal.address.p0(ptr align 4 @hidden)
int use_hidden() { return hidden; }

// CXX-DAG: declare dllimport void @_ZTH3dyn()
// CXX-DAG: declare dllimport void @_ZTH3obj()
// CXX-DAG: declare dllimport void @_ZTH3ref()
// CXX-DAG: declare extern_weak void @_ZTH8unmarked()
// CXX-DAG: [[UNLIKELY]] = !{!"branch_weights", i32 1, i32 1048575}
#endif

// CHECK-DAG: [[E]] = !{}
// CHECK-DAG: [[FOUR]] = !{i64 4}
