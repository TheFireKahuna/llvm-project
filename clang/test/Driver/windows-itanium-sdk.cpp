// REQUIRES: x86-registered-target

// Header and library discovery for Windows Itanium: clang's resource headers,
// the UCRT and the Windows SDK. No Visual Studio directory is ever searched.

// Defines that select the UCRT-hosted header configuration.
// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFINES %s
// DEFINES: "-cc1"
// DEFINES-SAME: "-D__MSVCRT__"
// DEFINES-SAME: "-DLLVM_CRT_UCRT"
// DEFINES-SAME: "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS"
// DEFINES-SAME: "-D_CRT_SECURE_NO_WARNINGS"
// DEFINES-SAME: "-UCLOCK_REALTIME"

// libc++ headers from a sysroot.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBCXX_SYSROOT %s
// LIBCXX_SYSROOT: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}c++{{.*}}v1"

// -nostdinc suppresses every system include.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdinc -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDINC %s
// NOSTDINC-NOT: "-internal-isystem"

// -nostdinc++ suppresses the C++ includes only.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -nostdinc++ -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDINCXX %s
// NOSTDINCXX-NOT: "{{.*}}c++{{.*}}v1"

// -nostdlibinc keeps the resource headers.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlibinc -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDLIBINC %s
// NOSTDLIBINC: "-internal-isystem" "{{.*}}lib{{.*}}clang{{.*}}include"
// NOSTDLIBINC-NOT: "{{.*}}ucrt"

// /imsvc adds explicit system include directories.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /imsvc "C:/SDK/include/ucrt" /imsvc "C:/SDK/include/um" /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=IMSVC %s
// IMSVC: "-cc1"
// IMSVC: "-internal-isystem" "C:/SDK/include/ucrt"
// IMSVC: "-internal-isystem" "C:/SDK/include/um"

// The Visual Studio include and library directories are never added, and the
// options that select a VC installation are unused.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /vctoolsdir "C:/VC" /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_VC %s
// NO_VC: warning: argument unused during compilation: '/vctoolsdir C:/VC'
// NO_VC-NOT: "-internal-isystem" "C:/VC{{.*}}"

// -L library paths.
// RUN: %clang --target=x86_64-unknown-windows-itanium \
// RUN:   -L/cross/x64/lib -L/cross/common/lib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CROSS_LIBPATH %s
// CROSS_LIBPATH: lld-link
// CROSS_LIBPATH: "-libpath:/cross/x64/lib"
// CROSS_LIBPATH: "-libpath:/cross/common/lib"

// Verbose output.
// RUN: %clang --target=x86_64-unknown-windows-itanium -v -c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=VERBOSE %s
// VERBOSE: Target: x86_64-unknown-windows-itanium
