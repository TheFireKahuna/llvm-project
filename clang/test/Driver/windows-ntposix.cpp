// REQUIRES: x86-registered-target

// Test NT-POSIX toolchain driver behavior.

// --- CC1 flags ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s
// CC1: "-triple" "x86_64-pc-windows-ntposix"
// CC1-DAG: "-fno-dllexport-inlines"
// CC1-DAG: "-fwchar-type=int"
// CC1-DAG: "-fsigned-wchar"
// CC1-DAG: "-pthread"
// CC1: "-exception-model=seh"
// CC1-NOT: "-fms-extensions"

// --- Override dllexport-inlines ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -fdllexport-inlines -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLLEXPORT %s
// DLLEXPORT-NOT: "-fno-dllexport-inlines"

// --- Override wchar ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -fshort-wchar -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=SHORT-WCHAR %s
// SHORT-WCHAR-NOT: "-fwchar-type=int"
// SHORT-WCHAR-NOT: "-fsigned-wchar"

// --- Linker: nostdlib (skips startup objects and default libs) ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTDLIB %s
// NOSTDLIB: lld-link
// NOSTDLIB-DAG: "-machine:x64"
// NOSTDLIB-DAG: "-subsystem:console"
// NOSTDLIB-DAG: "-nologo"
// NOSTDLIB-DAG: "-auto-import"
// NOSTDLIB-NOT: "-entry:mainCRTStartup"
// NOSTDLIB-NOT: "crt1.obj"
// NOSTDLIB-NOT: "c++.lib"
// NOSTDLIB-NOT: "unwind.lib"
// NOSTDLIB-NOT: "c.lib"
// NOSTDLIB-NOT: "kernel32.lib"
// NOSTDLIB-NOT: "-nodefaultlib:msvcrt"

// --- Linker: nodefaultlibs (keeps startup objects, skips libs) ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-DAG: "-machine:x64"
// NODEFAULTLIBS-DAG: "-nologo"
// NODEFAULTLIBS-DAG: "-auto-import"
// NODEFAULTLIBS-NOT: "c++.lib"
// NODEFAULTLIBS-NOT: "unwind.lib"
// NODEFAULTLIBS-NOT: "kernel32.lib"
// NODEFAULTLIBS-NOT: "-nodefaultlib:msvcrt"

// --- Subsystem: -mwindows ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -mwindows -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=GUI %s
// GUI: lld-link
// GUI: "-subsystem:windows"
// GUI-NOT: "-subsystem:console"

// --- Subsystem: -mconsole ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -mconsole -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s
// CONSOLE: lld-link
// CONSOLE: "-subsystem:console"
// CONSOLE-NOT: "-subsystem:windows"

// --- DLL (nostdlib to avoid file-not-found) ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -shared -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: lld-link
// DLL-DAG: "-dll"
// DLL-DAG: "-implib:{{.*}}.lib"
// DLL-DAG: "-machine:x64"
// DLL-NOT: "-subsystem:"
// DLL-NOT: "-entry:mainCRTStartup"

// --- DLL entry point (nostartfiles off, but nodefaultlibs to reduce deps) ---
// Note: DLL entry requires startup files which need sysroot; test with nostdlib.
// RUN: %clang --target=x86_64-pc-windows-ntposix -shared -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL-NOSTDLIB %s
// DLL-NOSTDLIB: lld-link
// DLL-NOSTDLIB-DAG: "-dll"
// DLL-NOSTDLIB-NOT: "-entry:_DllMainCRTStartup"
// DLL-NOSTDLIB-NOT: "-entry:mainCRTStartup"

// --- MSVC CRT blocking (nodefaultlib flags present when not -nostdlib) ---
// Note: With nodefaultlibs, the -nodefaultlib: flags are suppressed.
// Use nostartfiles to skip file checks but keep the CRT blocking.
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostartfiles -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=BLOCK-CRT %s
// BLOCK-CRT: lld-link
// BLOCK-CRT-DAG: "-nodefaultlib:msvcrt"
// BLOCK-CRT-DAG: "-nodefaultlib:msvcrtd"
// BLOCK-CRT-DAG: "-nodefaultlib:vcruntime"
// BLOCK-CRT-DAG: "-nodefaultlib:vcruntimed"
// BLOCK-CRT-DAG: "-nodefaultlib:ucrt"
// BLOCK-CRT-DAG: "-nodefaultlib:ucrtd"
// BLOCK-CRT-DAG: "-nodefaultlib:libcmt"
// BLOCK-CRT-DAG: "-nodefaultlib:libcmtd"
// BLOCK-CRT-DAG: "-nodefaultlib:oldnames"
// BLOCK-CRT-NOT: "-defaultlib:msvcrt"
// BLOCK-CRT-NOT: "-defaultlib:ucrt"
// BLOCK-CRT-NOT: "-defaultlib:user32"
// BLOCK-CRT-NOT: "-defaultlib:gdi32"
// BLOCK-CRT-NOT: "-defaultlib:advapi32"
// BLOCK-CRT-NOT: "-defaultlib:oldnames"

// --- -L paths ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -L/foo -L/bar -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBPATH %s
// LIBPATH: lld-link
// LIBPATH-DAG: "-libpath:/foo"
// LIBPATH-DAG: "-libpath:/bar"

// --- Output ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -o test.exe -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=OUTPUT %s
// OUTPUT: lld-link
// OUTPUT: "-out:test.exe"

// --- Reject non-lld linker ---
// RUN: not %clang --target=x86_64-pc-windows-ntposix -fuse-ld=link -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=REJECT-LD %s
// REJECT-LD: error: unsupported option '-fuse-ld=link (NT-POSIX requires lld-link)'

// --- Accept -fuse-ld=lld ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -fuse-ld=lld -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=ACCEPT-LLD %s
// ACCEPT-LLD: lld-link

// --- Default linker is lld-link ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DEFAULT-LD %s
// DEFAULT-LD: lld-link
// DEFAULT-LD-NOT: link.exe"
// DEFAULT-LD-NOT: ld"

// --- C-only link (no c++), with nostartfiles to avoid file checks ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostartfiles -### -x c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C-LINK %s
// C-LINK: lld-link
// C-LINK-NOT: "c++.lib"
