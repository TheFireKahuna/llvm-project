// REQUIRES: x86-registered-target

// Control Flow Guard on the Windows Itanium toolchain. Every x86-64 object
// records its EH continuation targets and every x86-64 image is marked
// shadow-stack compatible; the EH continuation table accompanies the guard
// tables. lld-link honours one -guard: argument, so the modes are combined.

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_CF %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=none -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_CF %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf -mguard=none -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_CF %s
// NO_CF: "-cc1"
// NO_CF-NOT: "-cfguard"
// NO_CF-NOT: "-cfguard-no-checks"
// NO_CF-SAME: "-ehcontguard"
// NO_CF: lld-link
// NO_CF-SAME: "-cetcompat"
// NO_CF-NOT: "-guard:

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF %s
// GUARD_CF: "-cc1"
// GUARD_CF-SAME: "-cfguard"
// GUARD_CF-SAME: "-ehcontguard"
// GUARD_CF: lld-link
// GUARD_CF-SAME: "-cetcompat"
// GUARD_CF-SAME: "-guard:cf,ehcont,exportsuppress"

// A DLL does not enable export suppression; the executable does.
// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF_DLL %s
// GUARD_CF_DLL: lld-link
// GUARD_CF_DLL-SAME: "-guard:cf,ehcont"
// GUARD_CF_DLL-NOT: exportsuppress

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf-nochecks -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_NOCHECKS %s
// GUARD_NOCHECKS: "-cc1"
// GUARD_NOCHECKS-SAME: "-cfguard-no-checks"
// GUARD_NOCHECKS: lld-link
// GUARD_NOCHECKS-SAME: "-guard:cf,ehcont,exportsuppress"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=ehcont -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_EHCONT %s
// GUARD_EHCONT: "-cc1"
// GUARD_EHCONT-NOT: "-cfguard"
// GUARD_EHCONT: lld-link
// GUARD_EHCONT-SAME: "-guard:ehcont"

// RUN: not %clang --target=x86_64-unknown-windows-itanium -mguard=invalid -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_INVALID %s
// GUARD_INVALID: error: unsupported argument 'invalid' to option '-mguard='

// AArch64 has no shadow stack marking, so no EH continuation table.
// RUN: %clang --target=aarch64-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF_ARM64 %s
// GUARD_CF_ARM64: "-cc1"
// GUARD_CF_ARM64-SAME: "-cfguard"
// GUARD_CF_ARM64-NOT: "-ehcontguard"
// GUARD_CF_ARM64: lld-link
// GUARD_CF_ARM64-NOT: "-cetcompat"
// GUARD_CF_ARM64-SAME: "-guard:cf,exportsuppress"
// RUN: %clang --target=aarch64-unknown-windows-itanium -mguard=cf -mguard=ehcont -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_EHCONT_ARM64 %s
// GUARD_EHCONT_ARM64: "-cc1"
// GUARD_EHCONT_ARM64-SAME: "-cfguard"
// GUARD_EHCONT_ARM64-SAME: "-ehcontguard"
// GUARD_EHCONT_ARM64: lld-link
// GUARD_EHCONT_ARM64-SAME: "-guard:cf,ehcont,exportsuppress"

// clang-cl spellings.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF %s
// SLASH_GUARD_CF: "-cc1"
// SLASH_GUARD_CF-SAME: "-cfguard"
// SLASH_GUARD_CF: lld-link
// SLASH_GUARD_CF-SAME: "-guard:cf,ehcont,exportsuppress"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf- -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF_DISABLE %s
// SLASH_GUARD_CF_DISABLE: "-cc1"
// SLASH_GUARD_CF_DISABLE-NOT: "-cfguard"
// SLASH_GUARD_CF_DISABLE: lld-link
// SLASH_GUARD_CF_DISABLE-NOT: "-guard:
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:ehcont /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_EHCONT %s
// SLASH_GUARD_EHCONT: "-cc1"
// SLASH_GUARD_EHCONT-SAME: "-ehcontguard"
