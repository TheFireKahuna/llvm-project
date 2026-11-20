// DEFINE: %{cc} = %clang_cc1 -emit-llvm -o - %s
// DEFINE: %{kcfi} = -fsanitize=kcfi -fsanitize-cfi-icall-generalize-pointers
// RUN: %{cc} -triple x86_64-unknown-windows-itanium | FileCheck %s --check-prefixes=CHECK,NOCHECKS,PLAIN
// RUN: %{cc} -triple aarch64-pc-windows-ntposix | FileCheck %s --check-prefixes=CHECK,NOCHECKS,PLAIN
// RUN: %{cc} -triple x86_64-pc-windows-ntposix %{kcfi} | FileCheck %s --check-prefixes=CHECK,CHECKS,GENERALIZED
// RUN: %{cc} -triple aarch64-unknown-windows-itanium %{kcfi} | FileCheck %s --check-prefixes=CHECK,CHECKS,GENERALIZED
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fsanitize-cfi-icall-experimental-normalize-integers \
// RUN:   -fpatchable-function-entry=4 -fpatchable-function-entry-offset=3 | FileCheck %s --check-prefix=NORMALIZED
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fsanitize-kcfi-hash=FNV-1a | FileCheck %s --check-prefix=FNV
// RUN: %{cc} -triple x86_64-unknown-linux-gnu -ffunction-type-prefix | FileCheck %s --check-prefixes=CHECK,NOCHECKS
// RUN: %{cc} -triple x86_64-unknown-windows-itanium -fno-function-type-prefix | FileCheck %s --check-prefix=NONE
// RUN: %{cc} -triple x86_64-w64-windows-gnu | FileCheck %s --check-prefix=NONE
// RUN: not %clang_cc1 -triple x86_64-unknown-windows-itanium -DOFFSET -emit-llvm -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=OFFSET
// RUN: %{cc} -triple i686-unknown-windows-itanium | FileCheck %s --check-prefix=NONE

/// On Windows Itanium and NT-POSIX every function that has a KCFI type
/// carries it, whether or not -fsanitize=kcfi checks calls: code in other
/// images reads the prefix. The module records the marker, which differs for
/// each option that changes the type identifiers, and the hash, by which LLVM
/// types the functions it creates; the object records the marker and the size
/// of the patchable prefix, so that objects of two variants do not link.

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

/// The size of the patchable prefix is one for the image, so an attribute
/// states no other than the unit's.
#ifdef OFFSET
// OFFSET: [[#@LINE+1]]:16: error: offset 1 of 'patchable_function_entry' attribute differs from the translation unit's offset 0, which every function type prefix has
__attribute__((patchable_function_entry(2, 1))) void f4(void) {}
#endif

// PLAIN:       !{i32 1, !"function-type-prefix", i32 661450628}
// GENERALIZED: !{i32 1, !"function-type-prefix", i32 119298566}
// CHECK:       !{i32 4, !"kcfi", i32 1}
// CHECK:       !{i32 4, !"kcfi-hash", !"xxHash64"}
// PLAIN:       !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_KCFI_PREFIX=276cef84.0\22"}
// GENERALIZED: !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_KCFI_PREFIX=071c5a06.0\22"}
// CHECKS:      ![[#TYPE]] = !{i32 [[#HASH]]}

// NORMALIZED:  !{i32 1, !"function-type-prefix", i32 -1606313516}
// NORMALIZED:  !{i32 4, !"kcfi-offset", i32 3}
// NORMALIZED:  !{!"/FAILIFMISMATCH:\22_WIN_ITANIUM_KCFI_PREFIX=a04199d4.3\22"}
// FNV:         !{i32 1, !"function-type-prefix", i32 -260265210}
// FNV:         !{i32 4, !"kcfi-hash", !"FNV-1a"}

// NONE-NOT: !kcfi_type
// NONE-NOT: KCFI_PREFIX
// NONE-NOT: !"kcfi
// NONE-NOT: !"function-type-prefix"
