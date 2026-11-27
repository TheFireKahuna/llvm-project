// DEFINE: %{cc} = %clang_cc1 -fcxx-exceptions -fexceptions -fms-extensions \
// DEFINE:   -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s
// RUN: %{cc} -triple aarch64-pc-windows-ntposix -fno-plt | FileCheck %s
// RUN: %{cc} -triple x86_64-pc-windows-ntposix -DRUNTIME | \
// RUN:   FileCheck %s --check-prefix=RUNTIME

// On Windows Itanium and NT-POSIX a runtime function that clang calls is a
// known import, in every PLT mode, whether the unit declares it before or
// after the call or not at all, and with or without a visibility. Only the
// runtime's own export, or a definition, says it is the image's.

#ifdef RUNTIME
#define VIS __declspec(dllexport)
#else
#define VIS __attribute__((visibility("default")))
#endif

extern "C" VIS void *__cxa_allocate_exception(unsigned long long);
void throws() { throw 1; }
extern "C" void __cxa_throw(void *, void *, void (*)(void *));
void *address = (void *)&__cxa_throw;

struct B { virtual ~B(); };
struct D : B {};
D *cast(B *b) { return dynamic_cast<D *>(b); }

// CHECK-DAG: declare dllimport ptr @__cxa_allocate_exception(i64)
// CHECK-DAG: declare dllimport void @__cxa_throw(ptr, ptr, ptr)
// CHECK-DAG: declare dllimport ptr @__dynamic_cast(ptr, ptr, ptr, i64)

// RUNTIME-DAG: declare ptr @__cxa_allocate_exception(i64)
// RUNTIME-DAG: declare dllimport void @__cxa_throw(ptr, ptr, ptr)
