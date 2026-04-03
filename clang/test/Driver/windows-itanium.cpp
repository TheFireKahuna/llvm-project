// REQUIRES: x86-registered-target

// Test Windows Itanium toolchain driver behavior.

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s
// CC1: "-triple" "x86_64-unknown-windows-itanium"
// CC1-DAG: "-D_LIBCPP_ABI_FORCE_ITANIUM"
// CC1-DAG: "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS"
// CC1-DAG: "-UCLOCK_REALTIME"
// CC1-DAG: "-fno-dllexport-inlines"
// CC1: "-exception-model=seh"

// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1-X86 %s
// CC1-X86: "-triple" "i686-unknown-windows-itanium"
// CC1-X86: "-exception-model=seh"

// RUN: %clang --target=x86_64-unknown-windows-itanium -D_LIBCPP_ABI_FORCE_ITANIUM=1 -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=USER-DEF %s
// USER-DEF: "-D" "_LIBCPP_ABI_FORCE_ITANIUM=1"

// RUN: %clang --target=x86_64-unknown-windows-itanium -U_LIBCPP_ABI_FORCE_ITANIUM -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=USER-UNDEF %s
// USER-UNDEF: "-U" "_LIBCPP_ABI_FORCE_ITANIUM"
// USER-UNDEF-NOT: "-D_LIBCPP_ABI_FORCE_ITANIUM"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fdllexport-inlines -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLLEXPORT-INLINES %s
// DLLEXPORT-INLINES-NOT: "-fno-dllexport-inlines"

// SEH is default; SJLJ is fallback. See windows-itanium-errors.c for warning tests.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fsjlj-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SJLJ-EXPLICIT %s
// SJLJ-EXPLICIT-NOT: warning:
// SJLJ-EXPLICIT: "-exception-model=sjlj"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SEH-EXPLICIT %s
// SEH-EXPLICIT-NOT: warning:
// SEH-EXPLICIT: "-exception-model=seh"

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
// LINK-DAG: "-defaultlib:kernel32"
// LINK-DAG: "-defaultlib:user32"
// LINK-DAG: "-defaultlib:gdi32"
// LINK-DAG: "-defaultlib:advapi32"
// LINK-DAG: "-defaultlib:oldnames"

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-X86 %s
// LINK-X86: lld-link
// LINK-X86: "-machine:x86"

// RUN: %clang --target=arm-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-ARM %s
// LINK-ARM: lld-link
// LINK-ARM: "-machine:arm"

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-ARM64 %s
// LINK-ARM64: lld-link
// LINK-ARM64: "-machine:arm64"

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: lld-link
// DLL-DAG: "-dll"
// DLL-DAG: "-implib:{{.*}}.lib"
// DLL-NOT: "-subsystem:"

// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIB %s
// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "-defaultlib:msvcrt"
// NOSTDLIB-NOT: "-defaultlib:ucrt"
// NOSTDLIB-NOT: "-defaultlib:kernel32"

// RUN: %clang --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-NOT: "-defaultlib:msvcrt"
// NODEFAULTLIBS-NOT: "-defaultlib:kernel32"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=EXPERIMENTAL %s
// EXPERIMENTAL: "-defaultlib:c++"
// EXPERIMENTAL: "-defaultlib:c++experimental"

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C-LINK %s
// C-LINK: lld-link
// C-LINK: "-defaultlib:unwind"
// C-LINK: "-defaultlib:msvcrt"
// C-LINK-NOT: "-defaultlib:c++"

// RUN: %clang --target=x86_64-unknown-windows-itanium -L/foo/bar -L/baz -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBPATH %s
// LIBPATH: lld-link
// LIBPATH-DAG: "-libpath:/foo/bar"
// LIBPATH-DAG: "-libpath:/baz"

// RUN: %clang --target=x86_64-unknown-windows-itanium -o myprogram.exe -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=OUTPUT %s
// OUTPUT: lld-link
// OUTPUT: "-out:myprogram.exe"

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -o mylib.dll -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL-OUTPUT %s
// DLL-OUTPUT: lld-link
// DLL-OUTPUT-DAG: "-out:mylib.dll"
// DLL-OUTPUT-DAG: "-dll"
// DLL-OUTPUT-DAG: "-implib:mylib.lib"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT-STDLIB %s
// DEFAULT-STDLIB: "-defaultlib:c++"
// DEFAULT-STDLIB-NOT: "-defaultlib:stdc++"
// DEFAULT-STDLIB-NOT: "-defaultlib:msvcprt"

// RUN: %clang --target=x86_64-scei-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SCEI %s
// SCEI: "-triple" "x86_64-scei-windows-itanium"
// SCEI: "-exception-model=seh"

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT-LINKER %s
// DEFAULT-LINKER: lld-link
// DEFAULT-LINKER-NOT: link.exe"
// DEFAULT-LINKER-NOT: ld"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=FUSE-LD-LLD %s
// FUSE-LD-LLD: lld-link

// RUN: %clangxx --target=x86_64-unknown-windows-itanium \
// RUN:   --sysroot=%S/Inputs/windows_itanium_tree -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SYSROOT %s
// SYSROOT: "-internal-isystem" "{{.*}}windows_itanium_tree{{.*}}include{{.*}}c++{{.*}}v1"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mwindows -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS-SUBSYSTEM %s
// WINDOWS-SUBSYSTEM: lld-link
// WINDOWS-SUBSYSTEM: "-subsystem:windows"
// WINDOWS-SUBSYSTEM-NOT: "-subsystem:console"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mconsole -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE-SUBSYSTEM %s
// CONSOLE-SUBSYSTEM: lld-link
// CONSOLE-SUBSYSTEM: "-subsystem:console"
// CONSOLE-SUBSYSTEM-NOT: "-subsystem:windows"

