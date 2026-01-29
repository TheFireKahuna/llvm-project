// REQUIRES: x86-registered-target

// Test security-related flags for Windows Itanium toolchain.
// This includes stack protection, ASLR, DEP, and other hardening features.

//===----------------------------------------------------------------------===//
// Stack protection: -fstack-protector
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fstack-protector -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STACK_PROTECTOR %s

// STACK_PROTECTOR: "-cc1"
// STACK_PROTECTOR-SAME: "-stack-protector" "1"

//===----------------------------------------------------------------------===//
// Stack protection: -fstack-protector-strong
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fstack-protector-strong -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STACK_STRONG %s

// STACK_STRONG: "-cc1"
// STACK_STRONG-SAME: "-stack-protector" "2"

//===----------------------------------------------------------------------===//
// Stack protection: -fstack-protector-all
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fstack-protector-all -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STACK_ALL %s

// STACK_ALL: "-cc1"
// STACK_ALL-SAME: "-stack-protector" "3"

//===----------------------------------------------------------------------===//
// MSVC-style /GS via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /GS /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GS %s

// /GS enables buffer security checks
// GS: "-cc1"
// GS-SAME: "-stack-protector" "2"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /GS- /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=GS_DISABLED %s

// /GS- disables buffer security checks
// GS_DISABLED: "-cc1"
// GS_DISABLED-NOT: "-stack-protector"

//===----------------------------------------------------------------------===//
// Safe Stack (not supported on Windows Itanium)
//===----------------------------------------------------------------------===//

// RUN: not %clang --target=x86_64-unknown-windows-itanium -fsanitize=safe-stack -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SAFESTACK %s

// Safe-stack is not supported on Windows Itanium (requires compiler-rt runtime)
// SAFESTACK: error: unsupported option '-fsanitize=safe-stack' for target 'x86_64-unknown-windows-itanium'

//===----------------------------------------------------------------------===//
// ASLR control via linker flags
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DYNAMICBASE -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASLR %s

// ASLR: lld-link
// ASLR-SAME: "/DYNAMICBASE"

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DYNAMICBASE:NO -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_ASLR %s

// NO_ASLR: lld-link
// NO_ASLR-SAME: "/DYNAMICBASE:NO"

//===----------------------------------------------------------------------===//
// High-entropy ASLR (64-bit only)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/HIGHENTROPYVA -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=HIGHENTROPYVA %s

// HIGHENTROPYVA: lld-link
// HIGHENTROPYVA-SAME: "/HIGHENTROPYVA"

//===----------------------------------------------------------------------===//
// DEP (Data Execution Prevention) via /NXCOMPAT
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/NXCOMPAT -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEP %s

// DEP: lld-link
// DEP-SAME: "/NXCOMPAT"

//===----------------------------------------------------------------------===//
// Spectre mitigation via -mspeculative-load-hardening
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -mspeculative-load-hardening -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SPECTRE %s

// Speculative load hardening is supported
// SPECTRE: "-cc1"
// SPECTRE-SAME: "-mspeculative-load-hardening"

// Note: /Qspectre is parsed but not implemented (silently ignored like MSVC driver)

//===----------------------------------------------------------------------===//
// /sdl (Security Development Lifecycle) via clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /sdl /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SDL %s

// /sdl implies /GS
// SDL: "-cc1"
// SDL-SAME: "-stack-protector" "2"

//===----------------------------------------------------------------------===//
// Combine multiple security features
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fstack-protector-strong \
// RUN:   -mguard=cf -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=COMBINED %s

// Check that both stack protection and CFG can be enabled together
// COMBINED: "-cc1"
// COMBINED-SAME: "-cfguard"
// COMBINED: "-stack-protector" "2"
// COMBINED: lld-link
// COMBINED-SAME: "-guard:cf"

//===----------------------------------------------------------------------===//
// Stack protection on different architectures
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -fstack-protector-strong -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STACK_X86 %s

// STACK_X86: "-cc1"
// STACK_X86-SAME: "-stack-protector" "2"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fstack-protector-strong -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STACK_ARM64 %s

// STACK_ARM64: "-cc1"
// STACK_ARM64-SAME: "-stack-protector" "2"
