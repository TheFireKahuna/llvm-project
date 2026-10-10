// The Windows Itanium toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// The runtime libraries are default libraries, after the inputs.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C %s \
// RUN:       --implicit-check-not=libc++ \
// RUN:       --implicit-check-not=-entry: \
// RUN:       --implicit-check-not=-subsystem: \
// RUN:       --implicit-check-not=-delayload-protect \
// RUN:       --implicit-check-not=-nodefaultlib:oldnames \
// RUN:       --implicit-check-not=heap=segment \
// RUN:       --implicit-check-not=user32 --implicit-check-not=gdi32 \
// RUN:       --implicit-check-not=advapi32 --implicit-check-not=shell32
// C:      lld-link{{(.exe)?}}" "-out:a.exe" "-machine:x64" "-nologo" "-lldignoreenv"
// C-SAME: "-manifest:embed"
// C-SAME: "-manifestinput:{{[^"]*}}segment_heap.manifest"
// C-SAME: "{{[^"]*}}.o"
// C-SAME: "-defaultlib:libunwind.dll.lib"
// C-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.wincrt{{(-x86_64)?}}.lib"
// C-SAME: "-defaultlib:clang_rt.wincrt_dynamic{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.ucrt_memory{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.aligned_alloc{{[^"]*}}.lib"
// C-SAME: "-defaultlib:ucrt.lib" "-defaultlib:kernel32.lib"
// C-SAME: "-defaultlib:ntdll.lib" "-defaultlib:oldnames.lib"
// C-SAME: "-defaultlib:onecore_apiset.lib"
// C-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// C-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// C-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// C-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:iso_stdio_wide_specifiers"

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

// -static links the runtimes' archives, wincrt's process-wide parts included,
// in place of their DLLs' import libraries.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -static 2>&1 \
// RUN:   | FileCheck --check-prefix=STATIC %s --implicit-check-not=.dll.lib \
// RUN:       --implicit-check-not=wincrt_dynamic
// STATIC:      "-defaultlib:libc++.lib" "-defaultlib:libunwind.lib"
// STATIC-SAME: "-defaultlib:clang_rt.wincrt{{(-x86_64)?}}.lib"
// STATIC-SAME: "-defaultlib:clang_rt.wincrt_static{{[^"]*}}.lib"

// A DLL's import library is named as the runtimes' are, beside its archive.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -shared \
// RUN:     -o foo.dll 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /LD /Fefoo.dll \
// RUN:     /Tc%s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s
// DLL: "-out:foo.dll" "-machine:x64" "-nologo" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"

// A GUI program links user32 by default; a console program does not, and
// neither links gdi32.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mwindows 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS %s --implicit-check-not=gdi32
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mconsole \
// RUN:     -mwindows 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS %s --implicit-check-not=gdi32
// WINDOWS: "-lldignoreenv" "-subsystem:windows"
// WINDOWS: "-defaultlib:onecore_apiset.lib" "-defaultlib:user32.lib"
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mconsole 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s \
// RUN:       --implicit-check-not=user32 --implicit-check-not=gdi32
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mwindows \
// RUN:     -mconsole 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s \
// RUN:       --implicit-check-not=user32 --implicit-check-not=gdi32
// CONSOLE: "-lldignoreenv" "-subsystem:console"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -g 2>&1 \
// RUN:   | FileCheck --check-prefix=DEBUG %s
// DEBUG: lld-link{{(.exe)?}}"
// DEBUG-SAME: "-debug"

// An image exports what its sources give default visibility, so -rdynamic
// passes nothing to the linker and is not reported as unused.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -rdynamic 2>&1 \
// RUN:   | FileCheck --check-prefix=RDYNAMIC %s \
// RUN:       --implicit-check-not=rdynamic \
// RUN:       --implicit-check-not=-export-all-symbols
// RDYNAMIC: lld-link{{(.exe)?}}"

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
// RUN:       --implicit-check-not=aligned_alloc \
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

// -l:file names the file exactly; one not in the library directories is left
// to lld-link.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -L%t/lib \
// RUN:     -l:qux.a -l:libfoo.dll.lib -l:other.lib 2>&1 \
// RUN:   | FileCheck --check-prefix=EXACT %s
// EXACT:      "{{[^"]*}}lib{{/|\\\\}}qux.a"
// EXACT-SAME: "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// EXACT-SAME: "other.lib"

// -l searches every library directory, wherever it is given, and those passed
// to the linker directly after the driver's own.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -lfoo -lbar \
// RUN:     -l:qux.a -llate -L%t/lib -Wl,-libpath:%t/late 2>&1 \
// RUN:   | FileCheck --check-prefix=LATE %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -lfoo -llate \
// RUN:     -Xlinker /LIBPATH:%t/lib -Xlinker -libpath:%t/late 2>&1 \
// RUN:   | FileCheck --check-prefix=LATE-XLINKER %s
// LATE:      "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// LATE-SAME: "{{[^"]*}}lib{{/|\\\\}}libbar.lib"
// LATE-SAME: "{{[^"]*}}lib{{/|\\\\}}qux.a"
// LATE-SAME: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"
// LATE-SAME: "-libpath:{{[^"]*}}late"
// LATE-XLINKER: "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// LATE-XLINKER: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"

// PE has no run-time library search path, so -rpath is dropped with its value.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -rpath /r1 \
// RUN:     -Wl,-rpath,/r2,-opt:ref -Xlinker -rpath -Xlinker /r4 \
// RUN:     -Xlinker -opt:icf -Wl,--rpath=/r3 2>&1 \
// RUN:   | FileCheck --check-prefix=RPATH %s --implicit-check-not=/r1 \
// RUN:       --implicit-check-not=/r2 --implicit-check-not=/r3 \
// RUN:       --implicit-check-not=/r4
// RPATH-COUNT-3: warning: ignoring '-rpath' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// RPATH:         warning: ignoring '--rpath' option as it is not currently supported for target 'x86_64-unknown-windows-itanium'
// RPATH:         lld-link{{(.exe)?}}"
// RPATH-SAME:    "-opt:ref" "-opt:icf"

// A HIP link takes the HIP runtime from the ROCm installation, as with the
// MSVC toolchain.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium --hip-link \
// RUN:     --rocm-path=%S/Inputs/rocm %s 2>&1 \
// RUN:   | FileCheck --check-prefix=HIP %s
// RUN: %clang -### --target=x86_64-unknown-windows-itanium --hip-link \
// RUN:     --rocm-path=%S/Inputs/rocm -no-hip-rt %s 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-HIP-RT %s --implicit-check-not=amdhip64
// HIP:       lld-link{{(.exe)?}}"
// HIP-SAME:  "-libpath:{{[^"]*}}Inputs{{/|\\\\}}rocm{{/|\\\\}}lib" "amdhip64.lib"
// NO-HIP-RT: lld-link{{(.exe)?}}"

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
//--- lib/qux.a
//--- late/liblate.dll.lib
//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
