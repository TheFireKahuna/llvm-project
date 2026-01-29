// REQUIRES: x86-registered-target

// Test Windows Itanium toolchain driver behavior.
// The Windows Itanium toolchain uses the Itanium C++ ABI on Windows with
// libc++, libc++abi, and libunwind, targeting COFF/PE with LLD.

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s
// CC1: "-triple" "x86_64-unknown-windows-itanium"
// CC1-DAG: "-D_LIBCPP_ABI_FORCE_ITANIUM"
// CC1-DAG: "-D_NO_CRT_STDIO_INLINE"
// CC1-DAG: "-UCLOCK_REALTIME"
// CC1-DAG: "-fno-dllexport-inlines"
// CC1: "-exception-model=seh"

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1-X86 %s
// CC1-X86: "-triple" "i686-unknown-windows-itanium"
// CC1-X86: "-exception-model=seh"

// Test user can override _LIBCPP_ABI_FORCE_ITANIUM with -D.
// RUN: %clang --target=x86_64-unknown-windows-itanium -D_LIBCPP_ABI_FORCE_ITANIUM=1 -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=USER-DEF %s
// User-supplied -D gets expanded as separate args.
// USER-DEF: "-D" "_LIBCPP_ABI_FORCE_ITANIUM=1"

// Test user can undefine _LIBCPP_ABI_FORCE_ITANIUM with -U.
// RUN: %clang --target=x86_64-unknown-windows-itanium -U_LIBCPP_ABI_FORCE_ITANIUM -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=USER-UNDEF %s
// USER-UNDEF: "-U" "_LIBCPP_ABI_FORCE_ITANIUM"
// USER-UNDEF-NOT: "-D_LIBCPP_ABI_FORCE_ITANIUM"

// Test user can override -fno-dllexport-inlines with -fdllexport-inlines.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fdllexport-inlines -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLLEXPORT-INLINES %s
// DLLEXPORT-INLINES-NOT: "-fno-dllexport-inlines"

// Exception model: SEH is default, SJLJ is supported as fallback.
// For comprehensive exception model warning tests, see windows-itanium-errors.c
// RUN: %clang --target=x86_64-unknown-windows-itanium -fsjlj-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SJLJ-EXPLICIT %s
// SJLJ-EXPLICIT-NOT: warning:
// SJLJ-EXPLICIT: "-exception-model=sjlj"

// SEH is accepted without warning (default)
// RUN: %clang --target=x86_64-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SEH-EXPLICIT %s
// SEH-EXPLICIT-NOT: warning:
// SEH-EXPLICIT: "-exception-model=seh"

// Test linker invocation for x86_64.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK %s
// LINK: lld-link
// LINK-DAG: "-auto-import"
// LINK-DAG: "-incremental:no"
// LINK-DAG: "-subsystem:console"
// LINK-DAG: "-machine:x64"
// LINK-DAG: "-defaultlib:c++"
// LINK-DAG: "-defaultlib:unwind"
// LINK-DAG: "-defaultlib:msvcrt"
// LINK-DAG: "-defaultlib:ucrt"
// LINK-DAG: "-defaultlib:legacy_stdio_definitions"
// LINK-DAG: "-defaultlib:kernel32"
// LINK-DAG: "-defaultlib:user32"
// LINK-DAG: "-defaultlib:gdi32"
// LINK-DAG: "-defaultlib:advapi32"
// LINK-DAG: "-defaultlib:oldnames"

// Test linker invocation for i686 (32-bit x86).
// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-X86 %s
// LINK-X86: lld-link
// LINK-X86: "-machine:x86"

// Test linker invocation for ARM targets.
// RUN: %clang --target=arm-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-ARM %s
// LINK-ARM: lld-link
// LINK-ARM: "-machine:arm"

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-ARM64 %s
// LINK-ARM64: lld-link
// LINK-ARM64: "-machine:arm64"

// Test shared library (DLL) builds (no subsystem for DLLs).
// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: lld-link
// DLL-DAG: "-dll"
// DLL-DAG: "-implib:{{.*}}.lib"
// DLL-NOT: "-subsystem:"

// Test -nostdlib suppresses default libraries.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIB %s
// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "-defaultlib:msvcrt"
// NOSTDLIB-NOT: "-defaultlib:ucrt"
// NOSTDLIB-NOT: "-defaultlib:kernel32"

// Test -nodefaultlibs suppresses default libraries.
// RUN: %clang --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-NOT: "-defaultlib:msvcrt"
// NODEFAULTLIBS-NOT: "-defaultlib:kernel32"

// Test -fexperimental-library adds c++experimental.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=EXPERIMENTAL %s
// EXPERIMENTAL: "-defaultlib:c++"
// EXPERIMENTAL: "-defaultlib:c++experimental"

// Test C compilation (no C++ libraries).
// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C-LINK %s
// C-LINK: lld-link
// C-LINK: "-defaultlib:unwind"
// C-LINK: "-defaultlib:msvcrt"
// C-LINK-NOT: "-defaultlib:c++"

// Test library search path (-L).
// RUN: %clang --target=x86_64-unknown-windows-itanium -L/foo/bar -L/baz -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBPATH %s
// LIBPATH: lld-link
// LIBPATH-DAG: "-libpath:/foo/bar"
// LIBPATH-DAG: "-libpath:/baz"

// Test output filename handling.
// RUN: %clang --target=x86_64-unknown-windows-itanium -o myprogram.exe -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=OUTPUT %s
// OUTPUT: lld-link
// OUTPUT: "-out:myprogram.exe"

// Test DLL with custom output name generates correct import library.
// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -o mylib.dll -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL-OUTPUT %s
// DLL-OUTPUT: lld-link
// DLL-OUTPUT-DAG: "-out:mylib.dll"
// DLL-OUTPUT-DAG: "-dll"
// DLL-OUTPUT-DAG: "-implib:mylib.lib"

// Test default C++ standard library is libc++.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT-STDLIB %s
// DEFAULT-STDLIB: "-defaultlib:c++"
// DEFAULT-STDLIB-NOT: "-defaultlib:stdc++"
// DEFAULT-STDLIB-NOT: "-defaultlib:msvcprt"

// Test PlayStation variant triple (SCEI).
// RUN: %clang --target=x86_64-scei-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SCEI %s
// SCEI: "-triple" "x86_64-scei-windows-itanium"
// SCEI: "-exception-model=seh"

// Test that LLD is the default linker.
// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT-LINKER %s
// DEFAULT-LINKER: lld-link
// DEFAULT-LINKER-NOT: link.exe"
// DEFAULT-LINKER-NOT: ld"

// Test -fuse-ld= is respected but LLD is recommended.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=FUSE-LD-LLD %s
// FUSE-LD-LLD: lld-link

// Test cross-compilation with sysroot.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SYSROOT %s
// SYSROOT: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}include{{.*}}c++{{.*}}v1"

// Test -mwindows flag for GUI applications.
// RUN: %clang --target=x86_64-unknown-windows-itanium -mwindows -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS-SUBSYSTEM %s
// WINDOWS-SUBSYSTEM: lld-link
// WINDOWS-SUBSYSTEM: "-subsystem:windows"
// WINDOWS-SUBSYSTEM-NOT: "-subsystem:console"

// Test -mconsole flag (explicit console subsystem).
// RUN: %clang --target=x86_64-unknown-windows-itanium -mconsole -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE-SUBSYSTEM %s
// CONSOLE-SUBSYSTEM: lld-link
// CONSOLE-SUBSYSTEM: "-subsystem:console"
// CONSOLE-SUBSYSTEM-NOT: "-subsystem:windows"

// For link.exe warning test, see windows-itanium-errors.c (LINK_EXE_WARN)
