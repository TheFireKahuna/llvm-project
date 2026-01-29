// REQUIRES: x86-registered-target

// Test Unicode support for Windows Itanium toolchain.
// This tests -municode flag which enables Unicode support in Windows programs.

//===----------------------------------------------------------------------===//
// -municode defines UNICODE macro
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MUNICODE %s

// -municode should define UNICODE for wide-character APIs
// MUNICODE: "-cc1"
// MUNICODE-SAME: "-DUNICODE"

//===----------------------------------------------------------------------===//
// Without -municode, no UNICODE define
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_MUNICODE %s

// NO_MUNICODE: "-cc1"
// NO_MUNICODE-NOT: "-DUNICODE"

//===----------------------------------------------------------------------===//
// User can manually define UNICODE
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -DUNICODE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MANUAL_UNICODE %s

// MANUAL_UNICODE: "-cc1"
// MANUAL_UNICODE: "-D" "UNICODE"

//===----------------------------------------------------------------------===//
// _UNICODE usually paired with UNICODE
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -DUNICODE -D_UNICODE -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=BOTH_UNICODE %s

// BOTH_UNICODE: "-cc1"
// BOTH_UNICODE: "-D" "UNICODE"
// BOTH_UNICODE: "-D" "_UNICODE"

//===----------------------------------------------------------------------===//
// Entry point: mainCRTStartup (not wmainCRTStartup)
// NOTE: Windows Itanium doesn't currently change entry point with -municode.
// The entry point remains mainCRTStartup regardless of -municode flag.
// This differs from MinGW which switches to wmainCRTStartup.
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ENTRY_POINT %s

// Entry point is always mainCRTStartup for Windows Itanium
// ENTRY_POINT: lld-link
// ENTRY_POINT-SAME: "-entry:mainCRTStartup"
// ENTRY_POINT-NOT: "-entry:wmainCRTStartup"

//===----------------------------------------------------------------------===//
// -municode with -shared (DLL)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -municode -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_UNICODE %s

// DLL entry point unchanged with -municode
// DLL_UNICODE: "-cc1"
// DLL_UNICODE-SAME: "-DUNICODE"
// DLL_UNICODE: lld-link
// DLL_UNICODE-SAME: "-entry:_DllMainCRTStartup"

//===----------------------------------------------------------------------===//
// Unicode on different architectures
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=X86_UNICODE %s

// X86_UNICODE: "-cc1"
// X86_UNICODE-SAME: "-DUNICODE"

// RUN: %clang --target=aarch64-unknown-windows-itanium -municode -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ARM64_UNICODE %s

// ARM64_UNICODE: "-cc1"
// ARM64_UNICODE-SAME: "-DUNICODE"

//===----------------------------------------------------------------------===//
// -mwindows combined with -municode
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -mwindows -municode -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WINDOWS_UNICODE %s

// WINDOWS_UNICODE: "-cc1"
// WINDOWS_UNICODE-SAME: "-DUNICODE"
// WINDOWS_UNICODE: lld-link
// WINDOWS_UNICODE-SAME: "-subsystem:windows"
