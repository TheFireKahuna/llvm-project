// The Windows Itanium toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// The runtime libraries are default libraries, after the inputs.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C %s \
// RUN:       --implicit-check-not=libc++ \
// RUN:       --implicit-check-not=-entry: \
// RUN:       --implicit-check-not=-subsystem: \
// RUN:       --implicit-check-not=-import-slots \
// RUN:       --implicit-check-not=-delayload-protect \
// RUN:       --implicit-check-not=-cetcompat \
// RUN:       --implicit-check-not=-guard: \
// RUN:       --implicit-check-not=-nodefaultlib:oldnames \
// RUN:       --implicit-check-not=heap=segment
// C:      lld-link{{(.exe)?}}" "-out:a.exe" "-machine:x64" "-nologo" "-lldignoreenv"
// C-SAME: "{{[^"]*}}.o"
// C-SAME: "-defaultlib:libunwind.dll.lib"
// C-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.wincrt{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.ucrt_memory{{[^"]*}}.lib"
// C-SAME: "-defaultlib:ucrt.lib" "-defaultlib:kernel32.lib"
// C-SAME: "-defaultlib:ntdll.lib" "-defaultlib:oldnames.lib"
// C-SAME: "-defaultlib:user32.lib" "-defaultlib:advapi32.lib"
// C-SAME: "-defaultlib:shell32.lib"
// C-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// C-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// C-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// C-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:iso_stdio_wide_specifiers"

// RUN: %clangxx -### --target=aarch64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CXX %s
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CXX %s
// CXX:      lld-link{{(.exe)?}}"
// CXX-SAME: "-machine:arm64"
// CXX-SAME: "-defaultlib:libc++.dll.lib" "-defaultlib:libunwind.dll.lib"

// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -fexperimental-library 2>&1 \
// RUN:   | FileCheck --check-prefix=EXPERIMENTAL %s
// EXPERIMENTAL: "-defaultlib:libc++.dll.lib" "-defaultlib:libc++experimental.lib"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -unwindlib=none 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-UNWIND %s \
// RUN:       --implicit-check-not=libunwind
// NO-UNWIND: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"

// A DLL's import library is named as the runtimes' are, beside its archive.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -shared \
// RUN:     -o foo.dll 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /LD /Fefoo.dll \
// RUN:     /Tc%s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: "-out:foo.dll" "-machine:x64" "-nologo" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mwindows 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS %s
// WINDOWS: "-lldignoreenv" "-subsystem:windows"
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mconsole 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s
// CONSOLE: "-lldignoreenv" "-subsystem:console"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -g 2>&1 \
// RUN:   | FileCheck --check-prefix=DEBUG %s
// DEBUG: lld-link{{(.exe)?}}"
// DEBUG-SAME: "-debug"

// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -nostdlib 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// NODEFAULTLIBS: lld-link{{(.exe)?}}"

// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -nolibc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOLIBC %s \
// RUN:       --implicit-check-not=wincrt \
// RUN:       --implicit-check-not=ucrt.lib
// NOLIBC: "-defaultlib:libc++.dll.lib"
// NOLIBC-SAME: "-nodefaultlib:msvcrt"

// -l finds the runtimes' "lib"-prefixed import library, then archive, and
// leaves any other name to lld-link.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -L%t/lib \
// RUN:     -lfoo -lbar -lkernel32 -lbaz.lib 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBS %s
// LIBS:      "-libpath:{{[^"]*}}lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libbar.lib"
// LIBS-SAME: "kernel32.lib" "baz.lib"

// The Universal CRT and Windows SDK libraries, found as the MSVC toolchain
// finds them.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=SDK-X64 %s
// SDK-X64:      "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt{{/|\\\\}}x64"
// SDK-X64-SAME: "-libpath:[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}um{{/|\\\\}}x64"
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=SDK-A64 %s
// SDK-A64:      "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt{{/|\\\\}}arm64"
// SDK-A64-SAME: "-libpath:[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}um{{/|\\\\}}arm64"

// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /Tc%s \
// RUN:     /link -opt:ref 2>&1 \
// RUN:   | FileCheck --check-prefix=SLASH-LINK %s
// SLASH-LINK: "-nodefaultlib:iso_stdio_wide_specifiers" "-opt:ref"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -fuse-ld=lld 2>&1 \
// RUN:   | FileCheck --check-prefix=LLD %s
// LLD: lld-link{{(.exe)?}}"
// RUN: not %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -fuse-ld=bfd 2>&1 \
// RUN:   | FileCheck --check-prefix=BFD %s
// BFD: error: unsupported option '-fuse-ld=bfd' for target 'x86_64-unknown-windows-itanium'

//--- lib/libfoo.dll.lib
//--- lib/libbar.lib
//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
