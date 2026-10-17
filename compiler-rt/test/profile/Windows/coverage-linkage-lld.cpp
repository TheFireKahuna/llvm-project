// REQUIRES: lld-available
/// With lld -opt:ref we can ensure discarded[01] and their profc/profd
/// variables are discarded.

// RUN: %clang_profgen -fcoverage-mapping -ffunction-sections -fuse-ld=lld -Wl,-debug:symtab,-opt:noref %S/coverage-linkage.cpp -o %t
// RUN: llvm-nm %t | FileCheck %s --check-prefix=NOGC
// RUN: %clang_profgen -fcoverage-mapping -ffunction-sections -fuse-ld=lld -Wl,-debug:symtab,-opt:ref %S/coverage-linkage.cpp -o %t
// RUN: llvm-nm %t | FileCheck %s --check-prefix=GC

/// Match both the Microsoft and the Itanium mangling, but not the alias
/// labels that name discarded[01] and survive the GC.
// NOGC:   T {{\?|_Z10}}discarded{{.*}}
// GC-NOT: T {{\?|_Z10}}discarded{{.*}}
