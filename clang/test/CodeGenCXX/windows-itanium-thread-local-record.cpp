// On Windows Itanium and NT-POSIX a thread-local definition states its guard,
// a thread-local byte not zero once a thread has initialized it, for the
// record that another image reaches it through. An exported definition exports
// its record and its initialization function, never the variable.

// DEFINE: %{cc} = %clang_cc1 -std=c++20 -fdeclspec \
// DEFINE:   -mdefault-visibility-export-mapping=explicit
// DEFINE: %{check} = FileCheck %s --implicit-check-not=dllexport \
// DEFINE:   --implicit-check-not=tls_record --implicit-check-not=export:
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - \
// RUN:   | %{check} --check-prefixes=CHECK,CXX
// RUN: %{cc} -triple aarch64-unknown-windows-ntposix -emit-llvm %s -o - \
// RUN:   | %{check} --check-prefixes=CHECK,CXX
// RUN: %clang_cc1 -x c -fdeclspec -mdefault-visibility-export-mapping=explicit \
// RUN:   -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - \
// RUN:   | %{check} --check-prefixes=CHECK,C

// A wrapper calls its unit's initialization, which an image may offer, through
// no alias.
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=WRAP
// WRAP-LABEL: define weak_odr hidden noundef ptr @_ZTW5vague()
// WRAP-NEXT:    call void @__tls_init()
// WRAP-LABEL: define weak_odr hidden noundef ptr @_ZTWN1XIiE6memberE()
// WRAP-NEXT:    call void @__cxx_global_var_init{{.*}}()

// The mapping that exports every definition exports every record.
// RUN: %clang_cc1 -std=c++20 -fdeclspec -mdefault-visibility-export-mapping=all \
// RUN:   -triple x86_64-unknown-windows-itanium -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=ALL

// No other target has records: MinGW exports no thread-local variable, and
// neither does a 32-bit target, where the attribute stays an error.
// RUN: %{cc} -triple x86_64-w64-windows-gnu -DNO_ATTRIBUTE -emit-llvm %s -o - \
// RUN:   | FileCheck %s --check-prefix=NONE --implicit-check-not=tls_record \
// RUN:     --implicit-check-not=__tls_set --implicit-check-not='$tls'
// RUN: not %{cc} -triple i686-unknown-windows-itanium -emit-llvm %s -o - 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ERR
// ERR: error: 'attr' cannot be thread local when declared 'dllexport'
// NONE: @constant = {{.*}}thread_local global i32 5

#define MARK __attribute__((visibility("default")))

// A definition that nothing initializes in a thread names __tls_set, which
// is always 1, and exports a function that does nothing for an importer that
// does not know it.
#ifdef __cplusplus
MARK thread_local int constant = 5;
#else
MARK _Thread_local int constant = 5;
#endif
// CHECK-DAG: @constant = dso_local thread_local global i32 5, align 4, !tls_record [[SET:![0-9]+]]
// CHECK-DAG: @__tls_set = linkonce_odr hidden thread_local constant i8 1, comdat, align 1
// C-DAG: @llvm.compiler.used = appending global [1 x ptr] [ptr @__tls_set]
// CHECK-DAG: define dso_local dllexport void @_ZTH8constant()
// CHECK-DAG: [[SET]] = !{ptr @__tls_set}
// CHECK-DAG: !{!"-export:constant$tls,data"}

// The attribute marks a definition too, in C++ and in C.
#ifndef NO_ATTRIBUTE
__declspec(dllexport) __thread int attr = 7;
#endif
// CHECK-DAG: @attr = dso_local thread_local global i32 7, align 4, !tls_record [[SET]]
// CHECK-DAG: define dso_local dllexport void @_ZTH4attr()
// CHECK-DAG: !{!"-export:attr$tls,data"}

// An unmarked definition states its guard and exports nothing; a hidden or
// internal one, which no other image reaches, states nothing.
__thread int unmarked;
__attribute__((visibility("hidden"))) __thread int hidden;
static __thread int internal;
int *use(void) { return &internal; }
// CHECK-DAG: @unmarked = dso_local thread_local global i32 0, align 4, !tls_record [[SET]]
// CHECK-DAG: @hidden = hidden thread_local global i32 0, align 4{{$}}
// CHECK-DAG: @{{.*}}internal = internal thread_local global i32 0, align 4{{$}}
// ALL-DAG: !{!"-export:unmarked$tls,data"}
// ALL-DAG: define dso_local dllexport void @_ZTH8unmarked()
// ALL-DAG: @hidden = hidden thread_local global i32 0, align 4{{$}}

#ifdef __cplusplus
struct S { S(); ~S(); int v; };
int f();

// The unit's ordered initialization runs under __tls_guard, which the records
// read and so the optimizer keeps as it is, and the initialization function
// that the unit has is the one exported.
MARK thread_local int ordered = f();
MARK thread_local S destroyed;
thread_local int unmarked_ordered = f();
// CXX-DAG: @ordered = dso_local thread_local global i32 0, align 4, !tls_record [[GUARD:![0-9]+]] [[DYNAMIC:#[0-9]+]]
// CXX-DAG: @destroyed = dso_local thread_local global %struct.S zeroinitializer, align 4, !tls_record [[GUARD]] [[DYNAMIC]]
// CXX-DAG: @unmarked_ordered = dso_local thread_local global i32 0, align 4, !tls_record [[GUARD]] [[DYNAMIC]]
// CXX-DAG: @__tls_guard = internal thread_local global i8 0
// CXX-DAG: @llvm.compiler.used = appending global [2 x ptr] [ptr @__tls_set, ptr @__tls_guard]
// CXX-DAG: @_ZTH7ordered = dso_local dllexport alias void (), ptr @__tls_init
// CXX-DAG: @_ZTH9destroyed = dso_local dllexport alias void (), ptr @__tls_init
// CXX-DAG: @_ZTH16unmarked_ordered = dso_local alias void (), ptr @__tls_init
// CXX-DAG: [[GUARD]] = !{ptr @__tls_guard}
// CXX-DAG: attributes [[DYNAMIC]] = { "dynamic-init"="thread-local" }
// CXX-DAG: !{!"-export:ordered$tls,data"}
// CXX-DAG: !{!"-export:destroyed$tls,data"}

// A variable with vague linkage is initialized under its own guard, which
// every copy of it names, in a unit that does not use the variable too. The
// images of a program share such a variable, so code reaches it and its
// guard through their records, the guard's naming the guard itself, and
// neither is DSO local.
template <class T> struct X { MARK static thread_local T member; };
template <class T> thread_local T X<T>::member = f();
template struct X<int>;
MARK inline thread_local int vague = f();
inline thread_local S unused;
MARK inline thread_local S unused_marked;
int use_vague() { return vague; }
// CXX-DAG: @_ZN1XIiE6memberE = weak_odr thread_local global i32 0, comdat, align 4, !tls_record [[OWN:![0-9]+]] [[DYNAMIC]]
// CXX-DAG: @_ZGVN1XIiE6memberE = weak_odr thread_local global i64 0, comdat($_ZN1XIiE6memberE), align 8, !tls_record [[OWN]]
// CXX-DAG: [[OWN]] = !{ptr @_ZGVN1XIiE6memberE}
// CXX-DAG: @_ZTHN1XIiE6memberE = weak_odr dllexport alias void (), ptr @__cxx_global_var_init
// CXX-DAG: @vague = linkonce_odr thread_local global i32 0, comdat, align 4, !tls_record [[VAGUE:![0-9]+]] [[DYNAMIC]]
// CXX-DAG: @_ZGV5vague = linkonce_odr thread_local global i64 0, comdat($vague), align 8, !tls_record [[VAGUE]]
// CXX-DAG: [[VAGUE]] = !{ptr @_ZGV5vague}
// CXX-DAG: @_ZTH5vague = linkonce_odr dllexport alias void (), ptr @__tls_init
// CXX-DAG: @unused = linkonce_odr thread_local global %struct.S zeroinitializer, comdat, align 4, !tls_record [[UNUSED:![0-9]+]] [[DYNAMIC]]
// CXX-DAG: @_ZGV6unused = linkonce_odr thread_local global i64 0, comdat($unused), align 8, !tls_record [[UNUSED]]
// CXX-DAG: [[UNUSED]] = !{ptr @_ZGV6unused}
// CXX-DAG: @_ZTH6unused = linkonce_odr alias void (), ptr @__tls_init
// CXX-DAG: !{!"-export:_ZN1XIiE6memberE$tls,data"}
// CXX-DAG: !{!"-export:vague$tls,data"}
// Whichever copy of an initialization function the link keeps initializes
// the variable, so a unit that does not use the variable exports its own.
// CXX-DAG: @_ZTH13unused_marked = linkonce_odr dllexport alias void (), ptr @__tls_init
// CXX-DAG: !{!"-export:unused_marked$tls,data"}

// So is a thread-local variable of an inline function, which no other unit
// declares.
inline int *local() {
  static thread_local int constant = 3;
  static thread_local int dynamic = f();
  return constant ? &constant : &dynamic;
}
int *use_local() { return local(); }
// CXX-DAG: @_ZZ5localvE8constant = linkonce_odr thread_local global i32 3, comdat, align 4, !tls_record [[SET]]
// CXX-DAG: @_ZZ5localvE7dynamic = linkonce_odr thread_local global i32 0, comdat, align 4, !tls_record [[LOCAL:![0-9]+]] [[DYNAMIC]]
// CXX-DAG: @_ZGVZ5localvE7dynamic = linkonce_odr thread_local global i64 0, comdat($_ZZ5localvE7dynamic), align 8, !tls_record [[LOCAL]]
// CXX-DAG: [[LOCAL]] = !{ptr @_ZGVZ5localvE7dynamic}
#endif
