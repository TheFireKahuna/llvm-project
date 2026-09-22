// REQUIRES: x86-registered-target

// Test NT-POSIX toolchain driver behavior.

// The C++ runtimes share Windows Itanium's library naming convention.
// RUN: %clangxx --target=x86_64-pc-windows-ntposix -nostartfiles -nolibc \
// RUN:   -fexperimental-library -### %s 2>&1 | FileCheck --check-prefix=LIBS %s
// LIBS: lld-link
// LIBS-SAME: "-defaultlib:libc++.dll.lib"
// LIBS-SAME: "-defaultlib:libc++experimental.lib"
// LIBS-SAME: "-defaultlib:libunwind.dll.lib"

// --- CC1 flags ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -c -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CC1 %s
// CC1: "-triple" "x86_64-pc-windows-ntposix"
// CC1-DAG: "-fno-dllexport-inlines"
// CC1-DAG: "-fwchar-type=int"
// CC1-DAG: "-fsigned-wchar"
// CC1-DAG: "-pthread"
// CC1-DAG: "-mdefault-visibility-export-mapping=explicit"
// CC1-DAG: "-fno-auto-import"
// CC1-DAG: "-fno-plt"
// CC1: "-exception-model=seh"
// CC1-NOT: "-fms-extensions"
// CC1-NOT: "-ehcontguard"
// CC1-NOT: "-D_DLL"

// --- Control Flow Guard: no shadow stack marking, so no EH continuation ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -mguard=cf -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=GUARD %s
// GUARD: "-cc1"
// GUARD-SAME: "-cfguard"
// GUARD-NOT: "-ehcontguard"
// GUARD: lld-link
// GUARD-NOT: "-cetcompat"
// GUARD-SAME: "-guard:cf,exportsuppress"

// --- Override dllexport-inlines ---
// RUN: %clang_cl --target=x86_64-pc-windows-ntposix /Zc:dllexportInlines /c -### -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLLEXPORT %s
// DLLEXPORT: "-cc1"
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
// NOSTDLIB-DAG: "-import-slots"
// NOSTDLIB-NOT: "-entry:mainCRTStartup"
// NOSTDLIB-NOT: "crt1.obj"
// NOSTDLIB-NOT: "libc++.dll.lib"
// NOSTDLIB-NOT: "libunwind.dll.lib"
// NOSTDLIB-NOT: "c.lib"
// NOSTDLIB-NOT: "kernel32.lib"

// --- Linker: nodefaultlibs (keeps startup objects, skips libs) ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostartfiles -nodefaultlibs -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s
// NODEFAULTLIBS: lld-link
// NODEFAULTLIBS-DAG: "-machine:x64"
// NODEFAULTLIBS-DAG: "-nologo"
// NODEFAULTLIBS-DAG: "-import-slots"
// NODEFAULTLIBS-NOT: "libc++.dll.lib"
// NODEFAULTLIBS-NOT: "libunwind.dll.lib"
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
// DLL-DAG: "-implib:{{.*}}.dll.lib"
// DLL-DAG: "-machine:x64"
// DLL-NOT: "-subsystem:"
// DLL-NOT: "-entry:mainCRTStartup"

// --- DLL entry point is an image property, present even without startup files ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -shared -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL-NOSTDLIB %s
// DLL-NOSTDLIB: lld-link
// DLL-NOSTDLIB-DAG: "-dll"
// DLL-NOSTDLIB-DAG: "-entry:_DllMainCRTStartup"
// DLL-NOSTDLIB-NOT: "-entry:mainCRTStartup"

// --- MSVC CRT blocking: the -nodefaultlib: directives are emitted unless
// -nodefaultlibs is given, so objects cannot pull an MSVC runtime through
// /DEFAULTLIB even under -nostdlib. ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -### %s 2>&1 \
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

// --- C-only link (no c++) ---
// RUN: %clang --target=x86_64-pc-windows-ntposix -nostdlib -### -x c %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C-LINK %s
// C-LINK: lld-link
// C-LINK-NOT: "libc++.dll.lib"
