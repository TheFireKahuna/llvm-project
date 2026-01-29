// REQUIRES: x86-registered-target

// Test MSVC compatibility flags for Windows Itanium toolchain.
// Windows Itanium uses MSVC headers, so it enables MS extensions by default.

//===----------------------------------------------------------------------===//
// Default: MS extensions and MS compatibility enabled
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT %s

// DEFAULT: "-cc1"
// DEFAULT-SAME: "-fms-extensions"
// DEFAULT-SAME: "-fms-compatibility"

//===----------------------------------------------------------------------===//
// MS extensions can be disabled
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-ms-extensions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_MS_EXT %s

// NO_MS_EXT: "-cc1"
// NO_MS_EXT-NOT: "-fms-extensions"

//===----------------------------------------------------------------------===//
// MS compatibility can be disabled
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-ms-compatibility -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_MS_COMPAT %s

// NO_MS_COMPAT: "-cc1"
// NO_MS_COMPAT-NOT: "-fms-compatibility"

//===----------------------------------------------------------------------===//
// -fno-rtti adds _HAS_STATIC_RTTI=0
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-rtti -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_RTTI %s

// NO_RTTI: "-cc1"
// NO_RTTI-SAME: "-D_HAS_STATIC_RTTI=0"

// RUN: %clang --target=x86_64-unknown-windows-itanium -frtti -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=RTTI %s

// RTTI: "-cc1"
// RTTI-NOT: "-D_HAS_STATIC_RTTI=0"

//===----------------------------------------------------------------------===//
// MSVC-style /O optimization flags - use clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /Od /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPT_OD %s

// OPT_OD: "-cc1"
// OPT_OD-SAME: "-O0"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /O1 /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPT_O1 %s

// OPT_O1: "-cc1"
// OPT_O1-SAME: "-Os"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /O2 /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPT_O2 %s

// OPT_O2: "-cc1"
// OPT_O2-SAME: "-O3"

//===----------------------------------------------------------------------===//
// MSVC-style /permissive flags - use clang_cl
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /permissive /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PERMISSIVE %s

// PERMISSIVE: "-cc1"
// PERMISSIVE-SAME: "-fno-operator-names"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /permissive- /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PERMISSIVE_MINUS %s

// /permissive- enables strict mode - no -fno-operator-names
// PERMISSIVE_MINUS: "-cc1"
// PERMISSIVE_MINUS-NOT: "-fno-operator-names"

//===----------------------------------------------------------------------===//
// MSVC-style -D with # (foo#bar -> foo=bar)
//===----------------------------------------------------------------------===//

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /DFOO#BAR /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFINE_HASH %s

// DEFINE_HASH: "-cc1"
// DEFINE_HASH-SAME: "-D" "FOO=BAR"

//===----------------------------------------------------------------------===//
// Default MSVC compatibility version
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MSVC_VERSION %s

// MSVC_VERSION: "-cc1"
// MSVC_VERSION-SAME: "-fms-compatibility-version=19.33"

//===----------------------------------------------------------------------===//
// Explicit MSVC compatibility version
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fms-compatibility-version=19.40 \
// RUN:   -c -### %s 2>&1 | FileCheck -check-prefix=MSVC_VERSION_EXPLICIT %s

// MSVC_VERSION_EXPLICIT: "-cc1"
// MSVC_VERSION_EXPLICIT-SAME: "-fms-compatibility-version=19.40"
