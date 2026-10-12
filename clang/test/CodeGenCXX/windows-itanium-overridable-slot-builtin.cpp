// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -emit-llvm -o - %s | FileCheck %s

// __builtin_overridable_default_slot gives the slot of a function marked as an
// overridable default, so libc++'s __is_function_overridden can read the bit
// for that slot in the executable's override table. It folds to the slot the
// attribute carries, whatever reaches it, and is a plain unsigned value.

typedef __SIZE_TYPE__ size_t;

__attribute__((overridable_default(0))) void *fnew(size_t);
__attribute__((overridable_default(9))) void fdel(void *, size_t);

// CHECK-LABEL: @_Z10slot_byref
// CHECK: ret i32 0
unsigned slot_byref() { return __builtin_overridable_default_slot(fnew); }

// CHECK-LABEL: @_Z10slot_byptr
// CHECK: ret i32 9
unsigned slot_byptr() { return __builtin_overridable_default_slot(&fdel); }

// The slot is an ordinary value, usable in an expression.
// CHECK-LABEL: @_Z3bitv
// CHECK: ret i32 512
unsigned bit() { return 1u << __builtin_overridable_default_slot(&fdel); }
