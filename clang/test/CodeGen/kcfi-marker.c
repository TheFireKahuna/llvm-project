// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -o - %s | FileCheck %s --check-prefixes=CHECK,NOCHECKS,MARKER
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -fsanitize=kcfi -o - %s | FileCheck %s --check-prefixes=CHECK,CHECKS,MARKER
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -o - %s | FileCheck %s --check-prefixes=CHECK,NOCHECKS,MARKER
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -fsanitize-cfi-icall-generalize-pointers -o - %s | FileCheck %s --check-prefix=GENERALIZED
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -fsanitize-cfi-icall-experimental-normalize-integers -o - %s | FileCheck %s --check-prefix=NORMALIZED
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -emit-llvm -fsanitize-kcfi-marker -fsanitize-kcfi-hash=FNV-1a -o - %s | FileCheck %s --check-prefix=FNV

/// With -fsanitize-kcfi-marker, functions carry KCFI types whether or not
/// -fsanitize=kcfi checks calls, and the module records the marker, which
/// differs for each option that changes the type identifiers.

typedef int (*fn_t)(int);

// CHECK: define dso_local i32 @f1(i32 noundef %x) #{{[0-9]+}} !kcfi_type ![[#TYPE:]]
int f1(int x) { return x; }

static int f2(int x) { return x + 1; }

/// A local function whose address is not taken has no type.
static int f3(int x) { return x + 2; }

fn_t get(void) { return f2; }

// CHECK: define internal i32 @f2(i32 noundef %x) #{{[0-9]+}} !kcfi_type ![[#TYPE]]

// CHECK-LABEL: define dso_local i32 @call(
// NOCHECKS:      call i32 %{{.*}}(i32 noundef 1){{$}}
// CHECKS:        call i32 %{{.*}}(i32 noundef 1) [ "kcfi"(i32 [[#%d,HASH:]]) ]
int call(fn_t f) { return f(1) + f3(2); }

// CHECK: define internal i32 @f3(i32 noundef %x) #{{[0-9]+}} {

// NOCHECKS-NOT: !"kcfi"
// CHECKS:       !{i32 4, !"kcfi", i32 1}
// MARKER:       !{i32 4, !"kcfi-marker", i32 661450628}
// CHECKS:       ![[#TYPE]] = !{i32 [[#HASH]]}
// GENERALIZED:  !{i32 4, !"kcfi-marker", i32 119298566}
// NORMALIZED:   !{i32 4, !"kcfi-marker", i32 -1606313516}
// FNV:          !{i32 4, !"kcfi-marker", i32 -260265210}
