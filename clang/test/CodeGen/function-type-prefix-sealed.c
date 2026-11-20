// DEFINE: %{cc} = %clang_cc1 -fsanitize=kcfi -fsanitize-kcfi-hash=FNV-1a -emit-llvm -o - %s
// RUN: %{cc} -triple x86_64-unknown-linux-gnu | FileCheck %s --check-prefixes=CHECK,PLAIN
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,PREFIX
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,PREFIX

/// Under function type prefixes type 0 is reserved for a function that no
/// pointer can reach, so no function or call has it. The type of void(void)
/// salted "j92a9il" hashes to 0 under FNV-1a; it is 1 instead, for the
/// function and the call alike.

#define SALT __attribute__((cfi_salt("j92a9il")))

typedef void (SALT *fn_t)(void);

// CHECK: define dso_local void @f() #{{[0-9]+}} !kcfi_type ![[#TYPE:]]
void SALT f(void) {}

fn_t get(void) { return f; }

// CHECK-LABEL: define dso_local void @call(
// PLAIN:         call void %{{.*}}() [ "kcfi"(i32 0) ]
// PREFIX:        call void %{{.*}}() [ "kcfi"(i32 1) ]
void call(fn_t p) { p(); }

// PLAIN:  ![[#TYPE]] = !{i32 0}
// PREFIX: ![[#TYPE]] = !{i32 1}
