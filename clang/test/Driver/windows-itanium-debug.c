// REQUIRES: x86-registered-target

// Test debug information options for Windows Itanium toolchain.

// RUN: %clang --target=x86_64-unknown-windows-itanium -g -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_G %s

// DEBUG_G: "-cc1"
// DEBUG_G-SAME: "-debug-info-kind=
// DEBUG_G: lld-link
// DEBUG_G-SAME: "-debug"

// RUN: %clang --target=x86_64-unknown-windows-itanium -g0 -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_G0 %s

// DEBUG_G0: "-cc1"
// DEBUG_G0-NOT: "-debug-info-kind=
// DEBUG_G0: lld-link
// DEBUG_G0-NOT: "-debug"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /Z7 /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_Z7 %s

// DEBUG_Z7: "-gcodeview"
// DEBUG_Z7-SAME: "-debug-info-kind=

// RUN: %clang --target=x86_64-unknown-windows-itanium -gline-tables-only -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_LINE %s

// DEBUG_LINE: "-cc1"
// DEBUG_LINE-SAME: "-debug-info-kind=line-tables-only"
// DEBUG_LINE: lld-link
// DEBUG_LINE-SAME: "-debug"

// RUN: %clang --target=x86_64-unknown-windows-itanium -g -fms-hotpatch -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_HOTPATCH %s

// DEBUG_HOTPATCH: lld-link
// DEBUG_HOTPATCH-SAME: "-debug"
// DEBUG_HOTPATCH-SAME: "-functionpadmin"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /Z7 /hotpatch /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_HOTPATCH_MSVC %s

// DEBUG_HOTPATCH_MSVC: "-cc1"
// DEBUG_HOTPATCH_MSVC-SAME: "-fms-hotpatch"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mno-incremental-linker-compatible \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=BREPRO %s

// BREPRO: lld-link
// BREPRO-SAME: "-Brepro"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mincremental-linker-compatible \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=NO_BREPRO %s

// NO_BREPRO: lld-link
// NO_BREPRO-NOT: "-Brepro"

// RUN: %clang --target=i686-unknown-windows-itanium -g -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_X86 %s

// DEBUG_X86: lld-link
// DEBUG_X86-SAME: "-machine:x86"
// DEBUG_X86-SAME: "-debug"

// RUN: %clang --target=aarch64-unknown-windows-itanium -g -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEBUG_ARM64 %s

// DEBUG_ARM64: lld-link
// DEBUG_ARM64-SAME: "-machine:arm64"
// DEBUG_ARM64-SAME: "-debug"
