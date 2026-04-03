// REQUIRES: x86-registered-target

// Test SDK and include path handling for Windows Itanium.

// Default defines.
// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFINES %s
// DEFINES: "-D_LIBCPP_ABI_FORCE_ITANIUM"
// DEFINES-SAME: "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS"
// DEFINES-SAME: "-D_CRT_SECURE_NO_WARNINGS"
// DEFINES-SAME: "-UCLOCK_REALTIME"

// libc++ includes via sysroot.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBCXX_SYSROOT %s
// LIBCXX_SYSROOT: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}c++{{.*}}v1"

// -nostdinc suppresses all system includes.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdinc -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDINC %s
// NOSTDINC-NOT: "-internal-isystem"

// -nostdinc++ suppresses C++ includes only.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -nostdinc++ -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDINCXX %s
// NOSTDINCXX-NOT: "{{.*}}c++{{.*}}v1"

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=RESOURCE_X86 %s
// RESOURCE_X86: "-resource-dir"

// /imsvc for system includes.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /imsvc "C:/SDK/include/ucrt" /imsvc "C:/SDK/include/um" /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=IMSVC %s
// IMSVC: "-cc1"
// IMSVC: "-internal-isystem" "C:/SDK/include/ucrt"
// IMSVC: "-internal-isystem" "C:/SDK/include/um"

// -L library paths.
// RUN: %clang --target=x86_64-unknown-windows-itanium \
// RUN:   -L/cross/x64/lib -L/cross/common/lib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=CROSS_LIBPATH %s
// CROSS_LIBPATH: lld-link
// CROSS_LIBPATH: "-libpath:/cross/x64/lib"
// CROSS_LIBPATH: "-libpath:/cross/common/lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -nostdinc -c -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_HOST_INCLUDES %s
// NO_HOST_INCLUDES: "-cc1"
// NO_HOST_INCLUDES-NOT: "-internal-isystem"

// /diasdkdir.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /diasdkdir "C:/Program Files/DIA SDK" /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DIASDK %s
// DIASDK: "-cc1"
// DIASDK: "-internal-isystem" "{{[^"]*}}DIA SDK{{.*}}include"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /diasdkdir "C:/DIA SDK" -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DIASDK_LINK %s
// DIASDK_LINK: lld-link
// DIASDK_LINK: "-libpath:{{[^"]*}}DIA SDK{{.*}}lib{{.*}}amd64"

// x86 uses legacy VC arch naming.
// RUN: %clang_cl --target=i686-unknown-windows-itanium \
// RUN:   /diasdkdir "C:/DIA SDK" -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DIASDK_LINK_X86 %s
// DIASDK_LINK_X86: lld-link
// DIASDK_LINK_X86: "-libpath:{{[^"]*}}DIA SDK{{.*}}lib"

// /winsysroot.
// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /winsysroot "C:/BuildTools" /c -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WINSYSROOT %s
// WINSYSROOT: "-cc1"
// WINSYSROOT: "-internal-isystem" "{{[^"]*}}DIA SDK{{.*}}include"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium \
// RUN:   /winsysroot "C:/BuildTools" -### -- %s 2>&1 \
// RUN:   | FileCheck -check-prefix=WINSYSROOT_LINK %s
// WINSYSROOT_LINK: lld-link
// WINSYSROOT_LINK: "-libpath:{{[^"]*}}DIA SDK{{.*}}lib{{.*}}amd64"

// Verbose output.
// RUN: %clang --target=x86_64-unknown-windows-itanium -v -c %s 2>&1 \
// RUN:   | FileCheck -check-prefix=VERBOSE %s
// VERBOSE: Target: x86_64-unknown-windows-itanium
