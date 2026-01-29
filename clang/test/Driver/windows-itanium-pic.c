// REQUIRES: x86-registered-target, aarch64-registered-target

// Test position-independent code (PIC) handling for Windows Itanium toolchain.
// On Windows, PIC/PIE semantics differ from Unix:
// - 64-bit targets (x64, ARM64) require PIC due to ABI constraints
// - 32-bit targets (x86) can use absolute addressing
// - ASLR is handled via /DYNAMICBASE at link time, not PIE

//===----------------------------------------------------------------------===//
// x86_64: PIC is mandatory (RIP-relative addressing)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X64_PIC %s

// x64 Windows requires PIC
// X64_PIC: "-cc1"
// X64_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-pic -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X64_NOPIC %s

// -fno-pic is ignored on x64 (PIC is forced)
// X64_NOPIC: "-cc1"
// X64_NOPIC-SAME: "-mrelocation-model" "pic"

//===----------------------------------------------------------------------===//
// i686: PIC is not required (direct addressing allowed)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_DEFAULT %s

// x86 defaults to static relocation model
// X86_DEFAULT: "-cc1"
// X86_DEFAULT-SAME: "-mrelocation-model" "static"

// RUN: %clang --target=i686-unknown-windows-itanium -fPIC -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_PIC %s

// -fPIC is accepted on x86
// X86_PIC: "-cc1"
// X86_PIC-SAME: "-mrelocation-model" "pic"

//===----------------------------------------------------------------------===//
// ARM64: PIC is mandatory (ADRP/ADD sequences)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_PIC %s

// ARM64 Windows requires PIC
// ARM64_PIC: "-cc1"
// ARM64_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fno-pic -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_NOPIC %s

// -fno-pic is ignored on ARM64 (PIC is forced)
// ARM64_NOPIC: "-cc1"
// ARM64_NOPIC-SAME: "-mrelocation-model" "pic"

//===----------------------------------------------------------------------===//
// PIE: Not a Windows concept
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fPIE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PIE_IGNORED %s

// PIE has no special meaning on Windows (ASLR uses /DYNAMICBASE)
// PIE_IGNORED: "-cc1"
// PIE_IGNORED-NOT: "-pic-is-pie"

//===----------------------------------------------------------------------===//
// Shared library (-shared) implies PIC
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SHARED_PIC %s

// SHARED_PIC: "-cc1"
// SHARED_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=i686-unknown-windows-itanium -shared -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SHARED_X86_PIC %s

// Even x86 uses PIC for DLLs
// SHARED_X86_PIC: "-cc1"
// SHARED_X86_PIC-SAME: "-mrelocation-model" "pic"

//===----------------------------------------------------------------------===//
// Code model (small vs large)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -mcmodel=small -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CMODEL_SMALL %s

// CMODEL_SMALL: "-cc1"
// CMODEL_SMALL-SAME: "-mcmodel=small"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mcmodel=large -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CMODEL_LARGE %s

// CMODEL_LARGE: "-cc1"
// CMODEL_LARGE-SAME: "-mcmodel=large"

//===----------------------------------------------------------------------===//
// ASLR control via linker (not compiler PIC)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DYNAMICBASE -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASLR_LINKER %s

// ASLR is controlled at link time, not via PIC
// ASLR_LINKER: lld-link
// ASLR_LINKER-SAME: "/DYNAMICBASE"
