// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fclang-abi-compat=23 -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-function-type-prefix -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple x86_64-w64-windows-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM

// On Windows Itanium a pure virtual slot names _purecall, the entry point that
// _set_purecall_handler applies to, as a vtable built by MSVC does. The image
// defines it, so it is not imported. A deleted virtual keeps the C++ runtime's
// entry point. Other Itanium targets, NT-POSIX among them, keep
// __cxa_pure_virtual, as does -fclang-abi-compat=23.

struct Abstract {
  virtual void pure() = 0;
  virtual void deleted() = delete;
  virtual ~Abstract();
};
Abstract::~Abstract() {}

// CHECK: @_ZTV8Abstract = {{.*}}ptr @_purecall, ptr @__cxa_deleted_virtual,
// CHECK-DAG: declare dso_local void @_purecall()
// CHECK-DAG: declare dllimport void @__cxa_deleted_virtual()

// ITANIUM: @_ZTV8Abstract = {{.*}}ptr @__cxa_pure_virtual, ptr @__cxa_deleted_virtual,
// ITANIUM-NOT: @_purecall
