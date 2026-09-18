// REQUIRES: x86-registered-target

// Control Flow Guard on the Windows Itanium toolchain.

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_CF %s
// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=none -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_CF %s
// NO_CF: "-cc1"
// NO_CF-NOT: "-cfguard"
// NO_CF-NOT: "-cfguard-no-checks"
// NO_CF: lld-link
// NO_CF-NOT: "-guard:cf"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF %s
// GUARD_CF: "-cc1"
// GUARD_CF-SAME: "-cfguard"
// GUARD_CF: lld-link
// GUARD_CF-SAME: "-guard:cf"
// GUARD_CF-NOT: "-guard:cf-"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mguard=cf-nochecks -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_NOCHECKS %s
// GUARD_NOCHECKS: "-cc1"
// GUARD_NOCHECKS-SAME: "-cfguard-no-checks"
// GUARD_NOCHECKS: lld-link
// GUARD_NOCHECKS-SAME: "-guard:cf"

// RUN: not %clang --target=x86_64-unknown-windows-itanium -mguard=invalid -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_INVALID %s
// GUARD_INVALID: error: unsupported argument 'invalid' to option '-mguard='

// RUN: %clang --target=aarch64-unknown-windows-itanium -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GUARD_CF_ARM64 %s
// GUARD_CF_ARM64: "-cc1"
// GUARD_CF_ARM64-SAME: "-cfguard"
// GUARD_CF_ARM64: lld-link
// GUARD_CF_ARM64-SAME: "-guard:cf"

// clang-cl spellings.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF %s
// SLASH_GUARD_CF: "-cc1"
// SLASH_GUARD_CF-SAME: "-cfguard"
// SLASH_GUARD_CF: lld-link
// SLASH_GUARD_CF-SAME: "-guard:cf"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:cf- -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_CF_DISABLE %s
// SLASH_GUARD_CF_DISABLE: "-cc1"
// SLASH_GUARD_CF_DISABLE-NOT: "-cfguard"
// SLASH_GUARD_CF_DISABLE: lld-link
// SLASH_GUARD_CF_DISABLE-SAME: "-guard:cf-"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /guard:ehcont /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SLASH_GUARD_EHCONT %s
// SLASH_GUARD_EHCONT: "-cc1"
// SLASH_GUARD_EHCONT-SAME: "-ehcontguard"
