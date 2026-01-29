// REQUIRES: x86-registered-target

// Test Control Flow Guard options for Windows Itanium toolchain.
// This mirrors mingw-cfguard.c for the Windows Itanium target.

//===----------------------------------------------------------------------===//
// Default: no CFG
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefixes=NO_CF,DEFAULT %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=none -### %s 2>&1 \
// RUN:   | FileCheck -check-prefixes=NO_CF,GUARD_NONE %s

// NO_CF: "-cc1"
// NO_CF-NOT: "-cfguard"
// NO_CF-NOT: "-cfguard-no-checks"
// NO_CF: lld-link
// NO_CF-NOT: "-guard:cf"
// DEFAULT-NOT: "-guard:cf-"
// GUARD_NONE-SAME: "-guard:cf-"

//===----------------------------------------------------------------------===//
// -mguard=cf: Enable CFG with checks
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF %s

// GUARD_CF: "-cc1"
// GUARD_CF-SAME: "-cfguard"
// GUARD_CF: lld-link
// GUARD_CF-SAME: "-guard:cf"
// GUARD_CF-NOT: "-guard:cf-"

//===----------------------------------------------------------------------===//
// -mguard=cf-nochecks: CFG table only, no runtime checks
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf-nochecks -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_NOCHECKS %s

// GUARD_NOCHECKS: "-cc1"
// GUARD_NOCHECKS-NOT: "-cfguard"
// GUARD_NOCHECKS-SAME: "-cfguard-no-checks"
// GUARD_NOCHECKS-NOT: "-cfguard"
// GUARD_NOCHECKS: lld-link
// GUARD_NOCHECKS-SAME: "-guard:cf"
// GUARD_NOCHECKS-NOT: "-guard:cf-"

//===----------------------------------------------------------------------===//
// Invalid -mguard value
//===----------------------------------------------------------------------===//

// RUN: not %clang --target=x86_64-unknown-windows-itanium -mguard=invalid -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_INVALID %s

// GUARD_INVALID: error: unsupported argument 'invalid' to option '-mguard='

//===----------------------------------------------------------------------===//
// Test i686 architecture
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF_X86 %s

// GUARD_CF_X86: "-cc1"
// GUARD_CF_X86-SAME: "-cfguard"
// GUARD_CF_X86: lld-link
// GUARD_CF_X86-SAME: "-guard:cf"

//===----------------------------------------------------------------------===//
// MSVC-style /guard: flags - use clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF %s

// SLASH_GUARD_CF: "-cc1"
// SLASH_GUARD_CF-SAME: "-cfguard"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf- /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF_DISABLE %s

// SLASH_GUARD_CF_DISABLE: "-cc1"
// SLASH_GUARD_CF_DISABLE-NOT: "-cfguard"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:ehcont /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_EHCONT %s

// SLASH_GUARD_EHCONT: "-cc1"
// SLASH_GUARD_EHCONT-SAME: "-ehcontguard"
