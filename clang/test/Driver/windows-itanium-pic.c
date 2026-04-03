// REQUIRES: x86-registered-target, aarch64-registered-target

// Test PIC handling for Windows Itanium. 64-bit targets require PIC;
// 32-bit can use static. PIE is not a Windows concept (ASLR uses /DYNAMICBASE).

// x64 requires PIC.
// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X64_PIC %s
// X64_PIC: "-cc1"
// X64_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-pic -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X64_NOPIC %s
// X64_NOPIC: "-cc1"
// X64_NOPIC-SAME: "-mrelocation-model" "pic"

// i686 defaults to static.
// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_DEFAULT %s
// X86_DEFAULT: "-cc1"
// X86_DEFAULT-SAME: "-mrelocation-model" "static"

// RUN: %clang --target=i686-unknown-windows-itanium -fPIC -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_PIC %s
// X86_PIC: "-cc1"
// X86_PIC-SAME: "-mrelocation-model" "pic"

// ARM64 requires PIC.
// RUN: %clang --target=aarch64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_PIC %s
// ARM64_PIC: "-cc1"
// ARM64_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=aarch64-unknown-windows-itanium -fno-pic -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_NOPIC %s
// ARM64_NOPIC: "-cc1"
// ARM64_NOPIC-SAME: "-mrelocation-model" "pic"

// PIE has no effect on Windows.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fPIE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PIE_IGNORED %s
// PIE_IGNORED: "-cc1"
// PIE_IGNORED-NOT: "-pic-is-pie"

// -shared implies PIC.
// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SHARED_PIC %s
// SHARED_PIC: "-cc1"
// SHARED_PIC-SAME: "-mrelocation-model" "pic"

// RUN: %clang --target=i686-unknown-windows-itanium -shared -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=SHARED_X86_PIC %s
// SHARED_X86_PIC: "-cc1"
// SHARED_X86_PIC-SAME: "-mrelocation-model" "pic"

// Code model.
// RUN: %clang --target=x86_64-unknown-windows-itanium -mcmodel=small -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CMODEL_SMALL %s
// CMODEL_SMALL: "-cc1"
// CMODEL_SMALL-SAME: "-mcmodel=small"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mcmodel=large -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CMODEL_LARGE %s
// CMODEL_LARGE: "-cc1"
// CMODEL_LARGE-SAME: "-mcmodel=large"

// ASLR via linker.
// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DYNAMICBASE -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASLR_LINKER %s
// ASLR_LINKER: lld-link
// ASLR_LINKER-SAME: "/DYNAMICBASE"
