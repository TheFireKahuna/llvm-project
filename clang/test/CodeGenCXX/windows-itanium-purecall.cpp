// DEFINE: %{cc} = %clang_cc1 -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefix=PURECALL
// RUN: %{cc} -triple aarch64-unknown-windows-itanium | FileCheck %s --check-prefix=PURECALL
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fclang-abi-compat=23 | FileCheck %s --check-prefix=ITANIUM
// RUN: %{cc} -triple x86_64-pc-windows-ntposix | FileCheck %s --check-prefix=ITANIUM

// On Windows Itanium a pure virtual slot names _purecall, the C runtime's
// entry point, which every image defines, so it is not imported. A deleted
// virtual function keeps the C++ runtime's entry point.

struct S {
  virtual void pure() = 0;
  virtual void deleted() = delete;
  virtual void key();
};
void S::key() {}

// PURECALL: @_ZTV1S = {{.*}}[ptr null, ptr @_ZTI1S, ptr @_purecall, ptr @__cxa_deleted_virtual, ptr @_ZN1S3keyEv]
// PURECALL: declare dso_local void @_purecall()
// PURECALL: declare dllimport void @__cxa_deleted_virtual()

// ITANIUM: @_ZTV1S = {{.*}}[ptr null, ptr @_ZTI1S, ptr @__cxa_pure_virtual, ptr @__cxa_deleted_virtual, ptr @_ZN1S3keyEv]
