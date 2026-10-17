// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -DFUNC -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -DMEMBER -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -DLOCAL -O1 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -DCODE -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE
// RUN: %clang_cc1 -triple x86_64-windows-msvc -fms-extensions -DCODE -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE

// Static data holds the address of a dllimport entity only because the linker
// has the loader write it there; a linker that does not would write a thunk's
// address for a function and fail on data. The object names a symbol that
// only such a linker defines, so that any other fails to link it. Code that
// loads the address through the import table needs nothing.

__declspec(dllimport) extern int imported_int;
__declspec(dllimport) int imported_func();
struct __declspec(dllimport) Foo {
  void method();
};

#if defined(FUNC)
int (*pfunc)() = imported_func;
#elif defined(MEMBER)
void (Foo::*pmethod)() = &Foo::method;
#elif defined(LOCAL)
void use(int (**out)[3]);
void local() {
  int (*table[3])() = {imported_func, imported_func, imported_func};
  use((int (**)[3])table);
}
#elif defined(CODE)
int *code() { return &imported_int; }
int (*code_func())() { return imported_func; }
#else
int *pint = &imported_int;
#endif

// CHECK: !llvm.linker.options = !{![[#OPT:]]}
// CHECK: ![[#OPT]] = !{!"/INCLUDE:__llvm_import_slots_v1"}
// NONE-NOT: __llvm_import_slots_v1
