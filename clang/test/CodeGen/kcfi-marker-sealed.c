// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fno-function-type-prefix -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -o - %s | FileCheck %s --check-prefixes=CHECK,PLAIN
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -o - %s | FileCheck %s --check-prefixes=CHECK,MARKER
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -o - %s | FileCheck %s --check-prefixes=CHECK,MARKER

/// A linker writes type 0 over the marked prefix of a function that no
/// pointer may reach, so with function type prefixes no function or call has
/// that type. The type of void(void) salted "j92a9il" hashes to 0 under
/// FNV-1a; with the marker it becomes 1, for the function and the call alike.

#define SALT __attribute__((cfi_salt("j92a9il")))

typedef void (SALT *fn_t)(void);

// CHECK: define dso_local void @f() #{{[0-9]+}} !kcfi_type ![[#TYPE:]]
void SALT f(void) {}

fn_t get(void) { return f; }

// CHECK-LABEL: define dso_local void @call(
// PLAIN:         call void %{{.*}}() [ "kcfi"(i32 0) ]
// MARKER:        call void %{{.*}}() [ "kcfi"(i32 1) ]
void call(fn_t p) { p(); }

// PLAIN:  ![[#TYPE]] = !{i32 0}
// MARKER: ![[#TYPE]] = !{i32 1}
