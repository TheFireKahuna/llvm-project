// The Windows Itanium toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// lld-link links, with the runtime libraries as default libraries after the
// inputs and no option that selects how imports bind. An executable embeds
// the segment-heap manifest shipped beside wincrt.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=C %s \
// RUN:       --implicit-check-not=libc++ --implicit-check-not=user32 \
// RUN:       --implicit-check-not=-entry: --implicit-check-not=-subsystem: \
// RUN:       --implicit-check-not=-nodefaultlib:oldnames \
// RUN:       --implicit-check-not=-auto-import \
// RUN:       --implicit-check-not=-runtime-pseudo-reloc \
// RUN:       --implicit-check-not=-import-slots \
// RUN:       --implicit-check-not=-start-stop-symbols \
// RUN:       --implicit-check-not=-boundary-symbols
// C:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv" "-manifest:embed"
// C-SAME: "-manifestinput:{{[^"]*}}segment_heap.manifest"
// C-SAME: "{{[^"]*}}.o"
// C-SAME: "-defaultlib:libunwind.dll.lib"
// C-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.wincrt{{(-x86_64)?}}.lib"
// C-SAME: "-defaultlib:clang_rt.wincrt_dynamic{{[^"]*}}.lib"
// C-SAME: "-defaultlib:clang_rt.ucrt_memory{{[^"]*}}.lib"
// C-SAME: "-defaultlib:ucrt.lib" "-defaultlib:kernel32.lib"
// C-SAME: "-defaultlib:ntdll.lib" "-defaultlib:oldnames.lib"
// C-SAME: "-defaultlib:onecore_apiset.lib"
// C-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// C-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// C-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// C-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:iso_stdio_wide_specifiers"

// A C++ link, which every clang-cl link may be, adds libc++.
// RUN: %clangxx -### --target=aarch64-unknown-windows-itanium %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CXX %s
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium -- %s 2>&1 \
// RUN:   | FileCheck --check-prefix=CXX %s
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -fexperimental-library 2>&1 \
// RUN:   | FileCheck --check-prefix=EXPERIMENTAL %s
// CXX:          "-defaultlib:libc++.dll.lib" "-defaultlib:libunwind.dll.lib"
// EXPERIMENTAL: "-defaultlib:libc++.dll.lib" "-defaultlib:libc++experimental.lib"

// libunwind is the platform's unwind library.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -unwindlib=none 2>&1 \
// RUN:   | FileCheck --check-prefix=NO-UNWIND %s --implicit-check-not=libunwind
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -unwindlib=platform 2>&1 \
// RUN:   | FileCheck --check-prefix=PLATFORM-UNWIND %s
// NO-UNWIND:       "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// PLATFORM-UNWIND: "-defaultlib:libunwind.dll.lib"

// -static links the runtimes' archives, wincrt's process-wide parts included,
// in place of their DLLs' import libraries.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -static 2>&1 \
// RUN:   | FileCheck --check-prefix=STATIC %s --implicit-check-not=.dll.lib \
// RUN:       --implicit-check-not=wincrt_dynamic
// STATIC:      "-defaultlib:libc++.lib" "-defaultlib:libunwind.lib"
// STATIC-SAME: "-defaultlib:clang_rt.wincrt_static{{[^"]*}}.lib"

// A DLL's import library is named as the runtimes' are, beside its archive,
// and a DLL embeds no manifest.
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s -shared \
// RUN:     -o foo.dll 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s --implicit-check-not=-manifest
// RUN: %clang_cl -### --target=x86_64-unknown-windows-itanium /LD /Fefoo.dll \
// RUN:     /Tc%s 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s --implicit-check-not=-manifest
// DLL: "-out:foo.dll" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"

// -mwindows and -mconsole select the subsystem, and a GUI program links
// user32.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -mconsole \
// RUN:     -mwindows 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s -mwindows \
// RUN:     -mconsole 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s --implicit-check-not=user32
// RUN: %clang -### --target=x86_64-unknown-windows-itanium -c %s -mwindows \
// RUN:     2>&1 | FileCheck --check-prefix=COMPILE-ONLY %s
// WINDOWS:      "-lldignoreenv" "-subsystem:windows"
// WINDOWS-SAME: "-defaultlib:onecore_apiset.lib" "-defaultlib:user32.lib"
// CONSOLE:      "-lldignoreenv" "-subsystem:console"
// COMPILE-ONLY-NOT: error:

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -g 2>&1 \
// RUN:   | FileCheck --check-prefix=DEBUG %s
// DEBUG: "-manifestinput:{{[^"]*}}" "-debug"

// -rdynamic passes nothing to the linker and is not reported as unused.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -rdynamic 2>&1 \
// RUN:   | FileCheck --check-prefix=RDYNAMIC %s --implicit-check-not=rdynamic \
// RUN:       --implicit-check-not=-export
// RDYNAMIC: lld-link{{(.exe)?}}"

// Without the default libraries an executable still embeds the manifest.
// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -nostdlib 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// RUN: %clangxx -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// NODEFAULTLIBS:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv"
// NODEFAULTLIBS-SAME: "-manifest:embed"
// NODEFAULTLIBS-SAME: "-manifestinput:{{[^"]*}}segment_heap.manifest"

// RUN: %clangxx -### --target=x86_64-unknown-windows-itanium %s -nolibc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOLIBC %s --implicit-check-not=wincrt \
// RUN:       --implicit-check-not=ucrt.lib
// NOLIBC:      "-defaultlib:libc++.dll.lib" "-defaultlib:libunwind.dll.lib"
// NOLIBC-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-nodefaultlib:msvcrt"

// -l finds the "lib"-prefixed import library, then archive, in every library
// directory, wherever it is given and those passed to the linker included; it
// leaves any other name, and the file -l: names, to lld-link. -static takes
// the archive.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -lfoo -lbar \
// RUN:     -lkernel32 -lbaz.lib -l:qux.a -llate -L%t/lib \
// RUN:     -Wl,-libpath:%t/late 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBS %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s -llate \
// RUN:     -Xlinker /LIBPATH:%t/late 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBS-XLINKER %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s -static -lfoo \
// RUN:     -L%t/lib 2>&1 | FileCheck --check-prefix=LIBS-STATIC %s
// LIBS:      "-libpath:{{[^"]*}}lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libbar.lib"
// LIBS-SAME: "kernel32.lib" "baz.lib" "qux.a"
// LIBS-SAME: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"
// LIBS-SAME: "-libpath:{{[^"]*}}late"
// LIBS-XLINKER: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"
// LIBS-STATIC: "{{[^"]*}}lib{{/|\\\\}}libfoo.lib"

// Linker options pass through unchanged, ELF's spellings included: lld-link
// takes the ones it implements.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -rpath /r1 \
// RUN:     -Wl,-rpath,/r2,--version-script=a.map -Xlinker -z -Xlinker defs \
// RUN:     -Wl,-opt:ref 2>&1 \
// RUN:   | FileCheck --check-prefix=PASS %s --implicit-check-not=warning:
// RUN: %clang_cl -### --target=aarch64-unknown-windows-itanium /Tc%s \
// RUN:     /link -opt:ref 2>&1 \
// RUN:   | FileCheck --check-prefix=SLASH-LINK %s
// PASS:       "-rpath" "/r1" "-rpath" "/r2" "--version-script=a.map" "-z" "defs" "-opt:ref"
// SLASH-LINK: "-nodefaultlib:iso_stdio_wide_specifiers" "-opt:ref"

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

// The Universal CRT's and the Windows SDK's libraries, found as the MSVC
// toolchain finds them.
// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=SDK -DARCH=x64 %s
// RUN: %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=SDK -DARCH=arm64 %s
// SDK:      "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt{{/|\\\\}}[[ARCH]]"
// SDK-SAME: "-libpath:[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}um{{/|\\\\}}[[ARCH]]"

// RUN: %clang -### --target=x86_64-unknown-windows-itanium %s -fuse-ld=lld 2>&1 \
// RUN:   | FileCheck --check-prefix=LLD %s
// RUN: not %clang -### --target=aarch64-unknown-windows-itanium %s \
// RUN:     -fuse-ld=bfd 2>&1 \
// RUN:   | FileCheck --check-prefix=BFD %s
// LLD: lld-link{{(.exe)?}}"
// BFD: error: unsupported option '-fuse-ld=bfd' for target 'aarch64-unknown-windows-itanium'

// The std module manifest is beside libc++'s import library.
// RUN: %clang -print-library-module-manifest-path -stdlib=libc++ \
// RUN:     --target=x86_64-unknown-windows-itanium -resource-dir=%t/lib \
// RUN:   | FileCheck --check-prefix=MANIFEST %s
// MANIFEST: lib{{/|\\}}libc++.modules.json

//--- lib/libfoo.dll.lib
//--- lib/libfoo.lib
//--- lib/libbar.lib
//--- lib/libc++.dll.lib
//--- lib/libc++.modules.json
//--- late/liblate.dll.lib
//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
