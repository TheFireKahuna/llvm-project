// REQUIRES: x86-registered-target

// Test LTO options for Windows Itanium toolchain.
// This mirrors mingw-lto.c for the Windows Itanium target.

//===----------------------------------------------------------------------===//
// Basic LTO with LLD (supported)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -flto -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LTO %s

// LTO: "-cc1"
// LTO-SAME: "-flto=full"
// LTO: lld-link
// LTO-NOT: error:

//===----------------------------------------------------------------------===//
// ThinLTO
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -flto=thin -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=THINLTO %s

// THINLTO: "-cc1"
// THINLTO-SAME: "-flto=thin"
// THINLTO: lld-link

//===----------------------------------------------------------------------===//
// LTO with sample profile
//===----------------------------------------------------------------------===//

// RUN: touch %t.prof
// RUN: %clang --target=x86_64-unknown-windows-itanium -flto \
// RUN:   -fprofile-sample-use=%t.prof -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LTO_PROFILE %s

// LTO_PROFILE: lld-link
// LTO_PROFILE-SAME: "-lto-sample-profile:{{.*}}.prof"

//===----------------------------------------------------------------------===//
// LTO with split DWARF
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -flto -gsplit-dwarf \
// RUN:   -o myprogram.exe -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LTO_SPLITDWARF %s

// LTO_SPLITDWARF: lld-link
// LTO_SPLITDWARF-SAME: "-dwodir:myprogram.exe_dwo"

//===----------------------------------------------------------------------===//
// LTO on i686
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -flto -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LTO_X86 %s

// LTO_X86: "-cc1"
// LTO_X86-SAME: "-flto=full"
// LTO_X86: lld-link
// LTO_X86-SAME: "-machine:x86"

//===----------------------------------------------------------------------===//
// LTO on ARM64
//===----------------------------------------------------------------------===//

// RUN: %clang --target=aarch64-unknown-windows-itanium -flto -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LTO_ARM64 %s

// LTO_ARM64: "-cc1"
// LTO_ARM64-SAME: "-flto=full"
// LTO_ARM64: lld-link
// LTO_ARM64-SAME: "-machine:arm64"
