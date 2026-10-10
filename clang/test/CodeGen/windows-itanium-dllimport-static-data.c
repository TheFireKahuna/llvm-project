// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -fdeclspec -DFUNC -emit-llvm -o - %s | FileCheck %s --check-prefix=NONE

// In C, static data that holds a dllimport variable's address names the
// symbol that only a linker that writes imported addresses there defines. A
// dllimport function's address is a constant on every target, the thunk's
// where the linker does not write it, so it needs nothing.

__declspec(dllimport) extern int imported_int;
__declspec(dllimport) int imported_func(void);

#ifdef FUNC
int (*pfunc)(void) = imported_func;
#else
int *pint = &imported_int;
#endif

// CHECK: !{!"/INCLUDE:__llvm_import_slots_v1"}
// NONE-NOT: __llvm_import_slots_v1
