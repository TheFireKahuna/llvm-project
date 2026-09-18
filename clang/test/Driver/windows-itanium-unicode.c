// REQUIRES: x86-registered-target

// Test -municode flag for Windows Itanium.

// -municode defines UNICODE.
// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MUNICODE %s
// MUNICODE: "-cc1"
// MUNICODE-SAME: "-DUNICODE"

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_MUNICODE %s
// NO_MUNICODE: "-cc1"
// NO_MUNICODE-NOT: "-DUNICODE"

// Manual UNICODE define.
// RUN: %clang --target=x86_64-unknown-windows-itanium -DUNICODE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MANUAL_UNICODE %s
// MANUAL_UNICODE: "-cc1"
// MANUAL_UNICODE: "-D" "UNICODE"

// RUN: %clang --target=x86_64-unknown-windows-itanium -DUNICODE -D_UNICODE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=BOTH_UNICODE %s
// BOTH_UNICODE: "-cc1"
// BOTH_UNICODE: "-D" "UNICODE"
// BOTH_UNICODE: "-D" "_UNICODE"

// Entry point unchanged with -municode (unlike MinGW).
// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ENTRY_POINT %s
// ENTRY_POINT: lld-link
// ENTRY_POINT-SAME: "-entry:mainCRTStartup"
// ENTRY_POINT-NOT: "-entry:wmainCRTStartup"

// DLL entry point.
// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_UNICODE %s
// DLL_UNICODE: "-cc1"
// DLL_UNICODE-SAME: "-DUNICODE"
// DLL_UNICODE: lld-link
// DLL_UNICODE-SAME: "-entry:_DllMainCRTStartup"

// Multi-architecture.
// RUN: %clang --target=i686-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_UNICODE %s
// X86_UNICODE: "-cc1"
// X86_UNICODE-SAME: "-DUNICODE"

// RUN: %clang --target=aarch64-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_UNICODE %s
// ARM64_UNICODE: "-cc1"
// ARM64_UNICODE-SAME: "-DUNICODE"

// -mwindows with -municode.
// RUN: %clang --target=x86_64-unknown-windows-itanium -mwindows -municode -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WINDOWS_UNICODE %s
// WINDOWS_UNICODE: "-cc1"
// WINDOWS_UNICODE-SAME: "-DUNICODE"
// WINDOWS_UNICODE: lld-link
// WINDOWS_UNICODE-SAME: "-subsystem:windows"
