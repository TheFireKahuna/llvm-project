// REQUIRES: x86-registered-target

// Test COFF dllexport/dllimport handling. Visibility flags must not convert
// native COFF semantics to ELF visibility.

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// CHECK-NOT: "-fvisibility-from-dllstorageclass"
// CHECK-NOT: "-fvisibility-dllexport=
// CHECK-NOT: "-fvisibility-nodllstorageclass=
// CHECK-NOT: "-fvisibility=hidden"
