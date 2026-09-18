// REQUIRES: x86-registered-target

// Windows Itanium toolchain: compile and link defaults.

// RUN: %clang --target=x86_64-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s
// CC1: "-triple" "x86_64-unknown-windows-itanium"
// CC1-DAG: "-D__MSVCRT__"
// CC1-DAG: "-DLLVM_CRT_UCRT"
// CC1-DAG: "-D_CRT_STDIO_ISO_WIDE_SPECIFIERS"
// CC1-DAG: "-UCLOCK_REALTIME"
// CC1-DAG: "-fno-dllexport-inlines"
// CC1: "-exception-model=seh"

// 32-bit x86 has no table-based SEH: SjLj is the model there.
// RUN: %clang --target=i686-unknown-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1-X86 %s
// CC1-X86: "-triple" "i686-unknown-windows-itanium"
// CC1-X86: "-exception-model=sjlj"

// RUN: %clang_cl --target=x86_64-unknown-windows-itanium /Zc:dllexportInlines /c -### -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLLEXPORT-INLINES %s
// DLLEXPORT-INLINES: "-cc1"
// DLLEXPORT-INLINES-NOT: "-fno-dllexport-inlines"

// SEH is the default; SjLj is accepted. See windows-itanium-errors.c for the
// rejected models.
// RUN: %clang --target=x86_64-unknown-windows-itanium -fsjlj-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SJLJ-EXPLICIT %s
// SJLJ-EXPLICIT-NOT: warning:
// SJLJ-EXPLICIT: "-exception-model=sjlj"
// RUN: %clang --target=x86_64-unknown-windows-itanium -fseh-exceptions -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SEH-EXPLICIT %s
// SEH-EXPLICIT-NOT: warning:
// SEH-EXPLICIT: "-exception-model=seh"

// The link line: lld-link, compiler-rt, wincrt's import libraries, and the
// MSVC runtime libraries blocked.
// RUN: %clangxx --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK %s
// LINK: lld-link
// LINK-DAG: "-machine:x64"
// LINK-DAG: "-subsystem:console"
// LINK-DAG: "-entry:mainCRTStartup"
// LINK-DAG: "c++.lib"
// LINK-DAG: "unwind.lib"
// LINK-DAG: "{{[^"]*}}clang_rt.builtins{{[^"]*}}.lib"
// LINK-DAG: "ucrt.lib"
// LINK-DAG: "kernel32.lib"
// LINK-DAG: "ntdll.lib"
// LINK-DAG: "-auto-import"
// LINK-DAG: "-nodefaultlib:msvcrt"
// LINK-DAG: "-nodefaultlib:vcruntime"
// LINK-DAG: "-nodefaultlib:libcmt"
// LINK-DAG: "-defaultlib:user32"
// LINK-DAG: "-lldignoreenv"
// LINK-NOT: "vcruntime.lib"
// LINK-NOT: "msvcrt.lib"

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-X86 %s
// LINK-X86: lld-link
// LINK-X86: "-machine:x86"

// RUN: %clang --target=aarch64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK-ARM64 %s
// LINK-ARM64: lld-link
// LINK-ARM64: "-machine:arm64"

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: lld-link
// DLL-DAG: "-dll"
// DLL-DAG: "-implib:{{[^"]*}}.lib"
// DLL-DAG: "-entry:_DllMainCRTStartup"
// DLL-NOT: "-subsystem:"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIB %s
// NOSTDLIB: lld-link
// NOSTDLIB-NOT: "-entry:"
// NOSTDLIB-NOT: "c++.lib"
// NOSTDLIB-NOT: "ucrt.lib"
// NOSTDLIB-NOT: "kernel32.lib"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-DAG: "-entry:mainCRTStartup"
// NODEFAULTLIBS-NOT: "c++.lib"
// NODEFAULTLIBS-NOT: "ucrt.lib"
// NODEFAULTLIBS-NOT: "-nodefaultlib:msvcrt"

// RUN: %clangxx --target=x86_64-unknown-windows-itanium -fexperimental-library -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=EXPERIMENTAL %s
// EXPERIMENTAL: "c++.lib"
// EXPERIMENTAL-SAME: "c++experimental.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -### -x c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C-LINK %s
// C-LINK: lld-link
// C-LINK-SAME: "unwind.lib"
// C-LINK-NOT: "c++.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -L/foo/bar -L/baz -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBPATH %s
// LIBPATH: lld-link
// LIBPATH-DAG: "-libpath:/foo/bar"
// LIBPATH-DAG: "-libpath:/baz"

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -o mylib.dll -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL-OUTPUT %s
// DLL-OUTPUT: lld-link
// DLL-OUTPUT-DAG: "-out:mylib.dll"
// DLL-OUTPUT-DAG: "-dll"
// DLL-OUTPUT-DAG: "-implib:mylib.lib"

// RUN: %clang --target=x86_64-scei-windows-itanium -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SCEI %s
// SCEI: "-triple" "x86_64-scei-windows-itanium"
// SCEI: "-exception-model=seh"

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
// WINDOWS-SUBSYSTEM-DAG: "-subsystem:windows"
// WINDOWS-SUBSYSTEM-DAG: "-entry:WinMainCRTStartup"
// WINDOWS-SUBSYSTEM-NOT: "-subsystem:console"

// RUN: %clang --target=x86_64-unknown-windows-itanium -mconsole -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE-SUBSYSTEM %s
// CONSOLE-SUBSYSTEM: lld-link
// CONSOLE-SUBSYSTEM: "-subsystem:console"
// CONSOLE-SUBSYSTEM-NOT: "-subsystem:windows"
