// REQUIRES: x86-registered-target

// Microsoft source compatibility on Windows Itanium: MSVC extensions are on
// for the UCRT and SDK headers, and the clang-cl argument translation is
// shared with the MSVC toolchain.

// The extensions come with the MSVC toolchain's default compatibility version,
// so clang applies the keyword extensions without pre-2015 MSVC quirks; the
// compatibility mode itself stays off.
// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT --implicit-check-not='"-fms-compatibility"' %s
// DEFAULT: "-cc1"
// DEFAULT-SAME: "-fms-extensions"
// DEFAULT-SAME: "-fms-compatibility-version=19.33"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-ms-extensions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_MS_EXT %s
// NO_MS_EXT: "-cc1"
// NO_MS_EXT-NOT: "-fms-extensions"
// NO_MS_EXT-NOT: "-fms-compatibility-version=

// RUN: %clang --target=x86_64-unknown-windows-itanium -fno-rtti -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_RTTI %s
// NO_RTTI: "-cc1"
// NO_RTTI-SAME: "-fno-rtti"

// clang-cl optimization flags.
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
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /O2 /Ob0 /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OPT_OB0 %s
// OPT_OB0: "-cc1"
// OPT_OB0-SAME: "-fno-inline"

// clang-cl permissive flags.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /permissive /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PERMISSIVE %s
// PERMISSIVE: "-cc1"
// PERMISSIVE-SAME: "-fno-operator-names"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /permissive- /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=PERMISSIVE_MINUS %s
// PERMISSIVE_MINUS: "-cc1"
// PERMISSIVE_MINUS-NOT: "-fno-operator-names"

// clang-cl define syntax: foo#bar becomes foo=bar.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /DFOO#BAR /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFINE_HASH %s
// DEFINE_HASH: "-cc1"
// DEFINE_HASH-SAME: "-D" "FOO=BAR"

// An explicit compatibility version replaces the default one.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fms-compatibility-version=19.40 \
// RUN:   -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MSVC_VERSION_EXPLICIT --implicit-check-not='"-fms-compatibility-version=19.33"' %s
// MSVC_VERSION_EXPLICIT: "-cc1"
// MSVC_VERSION_EXPLICIT-SAME: "-fms-compatibility-version=19.40"

// clang-cl /std: names a standard with MSVC's spellings; without it the target
// keeps clang's own default rather than MSVC's.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /std:c++20 /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STD_CXX20 %s
// STD_CXX20: "-cc1"
// STD_CXX20-SAME: "-std=c++20"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /std:c++latest /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STD_LATEST %s
// STD_LATEST: "-cc1"
// STD_LATEST-SAME: "-std=c++26"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=STD_DEFAULT --implicit-check-not='"-std=c++14"' %s
// STD_DEFAULT: "-cc1"

// clang-cl links the C++ standard library, as the clang++ driver does.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_LINK %s
// CL_LINK: lld-link
// CL_LINK-SAME: "-defaultlib:c++.lib"
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /clang:-nostdlib++ -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CL_LINK_NOSTDLIBXX %s
// CL_LINK_NOSTDLIBXX: lld-link
// CL_LINK_NOSTDLIBXX-NOT: "-defaultlib:c++.lib"

// Asynchronous exceptions: /EHa and -fasync-exceptions are accepted, as the
// Itanium call-site table can describe a range of instructions.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /EHa /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=EHA --implicit-check-not=warning: %s
// EHA: "-cc1"
// EHA-SAME: "-fcxx-exceptions"
// EHA-SAME: "-fexceptions"
// EHA-SAME: "-fasync-exceptions"
// RUN: %clang --target=x86_64-unknown-windows-itanium -fasync-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASYNC --implicit-check-not=warning: %s
// ASYNC: "-cc1"
// ASYNC-SAME: "-fasync-exceptions"
// RUN: %clang --target=x86_64-w64-windows-gnu -fasync-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASYNC_GNU %s
// ASYNC_GNU: "-cc1"
// ASYNC_GNU-NOT: "-fasync-exceptions"
