// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-auto-import -fno-plt -mdefault-visibility-export-mapping=explicit -fsanitize-cfi-icall-generalize-pointers -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fno-auto-import -mdefault-visibility-export-mapping=explicit -fsanitize-cfi-icall-generalize-pointers -emit-llvm -o - %s | FileCheck %s

/// A record of a single type gives a type fact in place of a node, named by
/// the type's precise identifier with its check identifier as the value, as a
/// function pointer parameter of that type gives. With pointer generalisation
/// the two identifiers differ.

typedef int (*cb)(int *);

struct one {
  cb f;
};

void def_direct(cb f) {}
void def_held(struct one o) {}

// CHECK:      module asm
// CHECK-NEXT: ".weak __kcfi_param_[[P:[0-9a-f]+]]_def_direct"
// CHECK-NEXT: ".set __kcfi_param_[[P]]_def_direct, [[C:[0-9]+]]"
// CHECK-NEXT: ".weak __kcfi_param_[[P]]_def_held"
// CHECK-NEXT: ".set __kcfi_param_[[P]]_def_held, [[C]]"
// CHECK-NOT:  __kcfi_
