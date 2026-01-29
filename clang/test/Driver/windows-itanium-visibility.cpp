// REQUIRES: x86-registered-target

// Test that Windows Itanium is configured correctly for COFF DLL exports.
//
// Windows Itanium produces COFF objects linked with LLD. For DLLs to work:
// - __declspec(dllexport) must generate /EXPORT directives in object files
// - __declspec(dllimport) must generate __imp_ references for IAT
// - The linker (lld-link) processes these to build import/export tables
//
// This requires preserving native COFF dllexport/dllimport semantics without
// converting them to ELF visibility (which would strip the COFF directives).

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck %s

// COFF dllexport semantics must be preserved (not converted to ELF visibility)
// CHECK-NOT: "-fvisibility-from-dllstorageclass"
// CHECK-NOT: "-fvisibility-dllexport=
// CHECK-NOT: "-fvisibility-nodllstorageclass=

// No forced hidden visibility (not meaningful for COFF, could confuse tooling)
// CHECK-NOT: "-fvisibility=hidden"
