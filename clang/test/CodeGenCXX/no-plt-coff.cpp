// Under -fno-plt on COFF a function the translation unit does not define is
// called through the import table: the declaration gets dllimport storage.
// The linker binds the reference directly when the definition is in the image.

// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fexceptions -fcxx-exceptions -fno-plt -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fexceptions -fcxx-exceptions -fno-plt -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=PLT

extern "C" void declared();
extern "C" void defined_later();
extern "C" __attribute__((visibility("hidden"))) void hidden();
extern "C" __attribute__((weak)) void weak();
extern "C" inline void inl() {}
extern "C" void (*take)();

extern "C" void use() {
  declared();
  defined_later();
  hidden();
  if (weak)
    weak();
  inl();
  take = &declared;
}

// A definition that follows its uses is not imported.
extern "C" void defined_later() {}

// Runtime functions the compiler calls take the import form too.
void thrower() { throw 1; }

// CHECK-DAG: define dso_local void @use()
// CHECK-DAG: define dso_local void @defined_later()
// CHECK-DAG: define linkonce_odr dso_local void @inl()
// CHECK-DAG: declare dllimport void @declared()
// CHECK-DAG: declare hidden void @hidden()
// CHECK-DAG: declare extern_weak void @weak()
// CHECK-DAG: declare dllimport void @{{__cxa_throw|_CxxThrowException}}(

// PLT: declare dso_local void @declared()
// PLT-NOT: dllimport
