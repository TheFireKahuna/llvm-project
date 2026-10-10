// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefixes=MARKER,WI --implicit-check-not=@__cxa_thread_atexit
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefixes=MARKER,WI --implicit-check-not=@__cxa_thread_atexit
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefixes=MARKER,NTPOSIX --implicit-check-not=@__cxa_thread_atexit
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefixes=MARKER,NTPOSIX --implicit-check-not=@__cxa_thread_atexit
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefix=PLAIN --implicit-check-not=@__llvm_kcfi_cxa
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-function-type-prefix -emit-llvm -fsanitize=kcfi -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefix=PLAIN --implicit-check-not=@__llvm_kcfi_cxa
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -fsanitize=kcfi -ffunction-type-prefix -fregister-global-dtors-with-atexit -o - %s | FileCheck %s --check-prefix=PLAIN --implicit-check-not=@__llvm_kcfi_cxa

/// Under the KCFI marker scheme, Windows Itanium and NT-POSIX register the
/// destructors and array helpers they pass to the runtime, which carry the
/// salted destructor type, through sibling entry points of __cxa_atexit and
/// __cxa_thread_atexit. A function with __attribute__((destructor)) carries
/// its own type and keeps the Itanium entry point, as does everything without
/// the marker scheme or on other targets.

struct S { ~S(); };
S s;
S arr[2];
thread_local S t;
S *use() { return &t; }
void local() { static S l; }
__attribute__((destructor)) void fini() {}

// MARKER-DAG: call i32 @__llvm_kcfi_cxa_atexit(ptr @_ZN1SD1Ev, ptr @s, ptr @__dso_handle)
// MARKER-DAG: call i32 @__llvm_kcfi_cxa_atexit(ptr @__cxx_global_array_dtor, ptr null, ptr @__dso_handle)
// MARKER-DAG: call i32 @__llvm_kcfi_cxa_thread_atexit(ptr @_ZN1SD1Ev, ptr @t, ptr @__dso_handle)
// MARKER-DAG: call i32 @__llvm_kcfi_cxa_atexit(ptr @_ZN1SD1Ev, ptr @_ZZ5localvE1l, ptr @__dso_handle)
// MARKER-DAG: call i32 @__cxa_atexit(ptr @_Z4finiv, ptr null, ptr @__dso_handle)

/// Windows Itanium images define the static registries themselves and import
/// the thread-local one from the C++ runtime.
// WI-DAG: declare dso_local i32 @__llvm_kcfi_cxa_atexit(ptr, ptr, ptr)
// WI-DAG: declare dllimport i32 @__llvm_kcfi_cxa_thread_atexit(ptr, ptr, ptr)
// NTPOSIX-DAG: declare dso_local i32 @__llvm_kcfi_cxa_atexit(ptr, ptr, ptr)
// NTPOSIX-DAG: declare dso_local i32 @__llvm_kcfi_cxa_thread_atexit(ptr, ptr, ptr)

// PLAIN-DAG: call i32 @__cxa_atexit(ptr @_ZN1SD1Ev, ptr @s, ptr @__dso_handle)
// PLAIN-DAG: call i32 @__cxa_atexit(ptr @__cxx_global_array_dtor, ptr null, ptr @__dso_handle)
// PLAIN-DAG: call i32 @__cxa_thread_atexit(ptr @_ZN1SD1Ev, ptr @t, ptr @__dso_handle)
// PLAIN-DAG: call i32 @__cxa_atexit(ptr @_ZN1SD1Ev, ptr @_ZZ5localvE1l, ptr @__dso_handle)
// PLAIN-DAG: call i32 @__cxa_atexit(ptr @_Z4finiv, ptr null, ptr @__dso_handle)
