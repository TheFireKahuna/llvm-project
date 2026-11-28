// On Windows Itanium and NT-POSIX a thread-local variable another image may
// define, one the unit only declares, is reached through the record of its
// image's place word, which the back end forms: the unit declares the
// variable, and a guard that names it, and takes their addresses with
// llvm.threadlocal.address, never the loader's TLS array through the thread
// pointer. A dllimport or explicit default visibility declaration is another
// image's; any other is of whichever image the link finds the variable in.

// DEFINE: %{cc} = %clang_cc1 -fdeclspec -pic-level 2 -emit-llvm -o - %s \
// DEFINE:   -mdefault-visibility-export-mapping=explicit
// DEFINE: %{check} = FileCheck %s --implicit-check-not='$tls' \
// DEFINE:   --implicit-check-not=@llvm.thread.pointer
// RUN: %{cc} -std=c++20 -triple x86_64-unknown-windows-itanium \
// RUN:   | %{check} --check-prefixes=CHECK,CXX
// RUN: %{cc} -std=c++20 -triple aarch64-unknown-windows-ntposix \
// RUN:   | %{check} --check-prefixes=CHECK,CXX
// RUN: %{cc} -x c -triple x86_64-unknown-windows-itanium | %{check}

// The option that reaches a declared variable directly leaves an unmarked
// declaration to this image, as does a visibility that is not default.
// RUN: %clang_cc1 -std=c++20 -triple x86_64-unknown-windows-itanium \
// RUN:   -fdeclspec -fdirect-access-external-data -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=DIRECT

#define MARK __attribute__((visibility("default")))
#define IMPORT __declspec(dllimport)

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

// CXX-DAG:    @dyn = external thread_local global i32
// CXX-DAG:    @"dyn$guard" = external thread_local global i8, !tls_record ![[DYN:[0-9]+]]
// CXX-DAG:    @obj = external thread_local global %struct.S
// CXX-DAG:    @"unmarked$guard" = external thread_local global i8, !tls_record ![[UNMARKED:[0-9]+]]
// CXX-DAG:    @hidden = external hidden thread_local global i32
// CHECK-DAG:  @imported = external thread_local global i32
// CHECK-DAG:  @marked = external thread_local global i32
// CHECK-DAG:  @plain = external thread_local global i32
// DIRECT-DAG: @unmarked = external dso_local thread_local global i32
// DIRECT-DAG: @plain = external dso_local thread_local global i32

// A variable that needs no initialization in a thread, as none of C does, is
// its address.
// CHECK-LABEL: define {{.*}} @{{.*}}use_marked
// CHECK:       call ptr @llvm.threadlocal.address.p0(ptr @marked)
int *use_marked(void) { return &marked; }

// CHECK-LABEL: define {{.*}} @{{.*}}use_imported
// CHECK:       call ptr @llvm.threadlocal.address.p0(ptr @imported)
// CHECK-NOT:   call void
int use_imported(void) { return imported; }

// CHECK-LABEL: define {{.*}} @{{.*}}use_plain
// CHECK:       call {{.*}}ptr @llvm.threadlocal.address.p0(ptr {{.*}}@plain)
// CHECK-NOT:   call void
int use_plain(void) { return plain; }

#ifdef __cplusplus
// CXX-LABEL: define {{.*}} @_Z12use_constantv
// CXX:       call ptr @llvm.threadlocal.address.p0(ptr @constant)
// CXX-NOT:   call {{.*}} @_ZTW
int use_constant() { return constant; }

// Any other variable goes through its wrapper, which tests the guard byte
// and calls the initialization function that the other image exports with
// the record, real or empty.
// CXX-LABEL: define {{.*}} @_Z7use_dynv
// CXX:       call ptr @_ZTW3dyn()
int use_dyn() { return dyn; }
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW3dyn() {{.*}} comdat {
// CXX:       [[G:%.*]] = call ptr @llvm.threadlocal.address.p0(ptr @"dyn$guard")
// CXX-NEXT:  [[V:%.*]] = call ptr @llvm.threadlocal.address.p0(ptr @dyn)
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
// CXX:       [[R:%.*]] = call ptr @llvm.threadlocal.address.p0(ptr @ref)
// CXX:       call void @_ZTH3ref()
// CXX:       [[P:%.*]] = load ptr, ptr [[R]], align 8
// CXX-NEXT:  ret ptr [[P]]

// The definition of an unmarked declaration, wherever the link finds it, may
// have no initialization function, so the reference to it is weak. Its guard
// is then never unset, and the wrapper is that of a known import.
// CXX-LABEL: define linkonce_odr hidden noundef ptr @_ZTW8unmarked()
// CXX:       call ptr @llvm.threadlocal.address.p0(ptr @"unmarked$guard")
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
// CXX-DAG: ![[DYN]] = !{ptr @dyn}
// CXX-DAG: ![[UNMARKED]] = !{ptr @unmarked}
#endif
