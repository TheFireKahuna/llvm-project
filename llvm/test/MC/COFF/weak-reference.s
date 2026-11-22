// A weak reference, a weak symbol with no definition, is a weak external
// whose default is null. Windows Itanium and NT-POSIX bind it as ELF binds a
// weak undefined symbol, which a NOLIBRARY weak external records: a strong
// reference from another object prevails over the default. A weak alias, a
// weak symbol with an absolute value, and a weak reference in an
// MSVC-environment object, are ALIAS weak externals.
// .weak_definition is a NOLIBRARY weak external whose default is the
// definition, on every triple.

// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-itanium %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s -DSEARCH="NoLibrary (0x1)"
// RUN: llvm-mc -filetype=obj -triple aarch64-unknown-windows-itanium %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s -DSEARCH="NoLibrary (0x1)"
// RUN: llvm-mc -filetype=obj -triple x86_64-unknown-windows-ntposix %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s -DSEARCH="NoLibrary (0x1)"
// RUN: llvm-mc -filetype=obj -triple x86_64-pc-windows-msvc %s \
// RUN:   | llvm-readobj --symbols - | FileCheck %s -DSEARCH="Alias (0x3)"

    .data
    .globl target
target:
    .long reference

    .weak reference

    .weak alias
alias = target

    .weak absolute
absolute = 42

    .weak_definition weakdef
weakdef:
    .long 0

// CHECK:      Name: reference
// CHECK:      StorageClass: WeakExternal
// CHECK:      Linked: .weak.reference.default.target
// CHECK-NEXT: Search: [[SEARCH]]

// CHECK:      Name: .weak.reference.default.target
// CHECK-NEXT: Value: 0
// CHECK-NEXT: Section: IMAGE_SYM_ABSOLUTE

// CHECK:      Name: alias
// CHECK:      StorageClass: WeakExternal
// CHECK:      Linked: target
// CHECK-NEXT: Search: Alias (0x3)

// CHECK:      Name: absolute
// CHECK:      StorageClass: WeakExternal
// CHECK:      Linked: .weak.absolute.default.target
// CHECK-NEXT: Search: Alias (0x3)

// CHECK:      Name: .weak.absolute.default.target
// CHECK-NEXT: Value: 42
// CHECK-NEXT: Section: IMAGE_SYM_ABSOLUTE

// CHECK:      Name: weakdef
// CHECK:      Linked: .weak.weakdef.default
// CHECK-NEXT: Search: NoLibrary (0x1)
