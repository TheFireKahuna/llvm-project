// The Windows Itanium toolchain: linking.

// RUN: rm -rf %t && split-file %s %t
// DEFINE: %{x64} = %clang -### --target=x86_64-unknown-windows-itanium %s
// DEFINE: %{a64} = %clang -### --target=aarch64-unknown-windows-itanium %s
// DEFINE: %{cxx-x64} = %clangxx -### --target=x86_64-unknown-windows-itanium %s
// DEFINE: %{cl-a64} = %clang_cl -### --target=aarch64-unknown-windows-itanium

// lld-link links, with the runtime libraries as default libraries after the
// inputs and no option that selects how imports bind. An executable embeds
// the segment-heap manifest shipped beside wincrt. The image's timestamps are
// a hash of its contents.
// RUN: %{x64} 2>&1 | FileCheck --check-prefix=C %s \
// RUN:   --implicit-check-not=libc++ --implicit-check-not=user32 --implicit-check-not=-entry: \
// RUN:   --implicit-check-not=-subsystem: --implicit-check-not=-nodefaultlib:oldnames \
// RUN:   --implicit-check-not=-auto-import --implicit-check-not=-runtime-pseudo-reloc \
// RUN:   --implicit-check-not=-import-slots --implicit-check-not=-start-stop-symbols \
// RUN:   --implicit-check-not=-boundary-symbols --implicit-check-not=_load_config_used
// C:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv" "-manifest:embed"
// C-SAME: "-manifestinput:{{[^"]*}}segment_heap.manifest" "-Brepro"
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

// A C++ link, which every clang-cl link may be, adds libc++; libunwind is
// the platform's unwind library; /link passes what follows to the linker.
// RUN: %{cxx-x64} -fexperimental-library -unwindlib=platform 2>&1 | FileCheck --check-prefixes=CXX,EXPERIMENTAL %s
// RUN: %{cl-a64} /Tc%s /link -opt:ref 2>&1 | FileCheck --check-prefixes=CXX,SLASH-LINK %s
// RUN: %{x64} -unwindlib=none -mguard=none 2>&1 | FileCheck --check-prefixes=NO-UNWIND %s \
// RUN:   --implicit-check-not=libunwind
// CXX:               "-defaultlib:libc++.dll.lib"
// EXPERIMENTAL-SAME: "-defaultlib:libc++experimental.lib"
// CXX-SAME:          "-defaultlib:libunwind.dll.lib"
// SLASH-LINK-SAME: "-nodefaultlib:iso_stdio_wide_specifiers" "-opt:ref"
// NO-UNWIND:       "-manifestinput:{{[^"]*}}" "-Brepro" "-cetcompat" "-guard:ehcont,nocf"
// NO-UNWIND-SAME:  "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"

// -static links the runtimes' archives, wincrt's process-wide parts included,
// in place of their DLLs' import libraries.
// RUN: %{cxx-x64} -static 2>&1 | FileCheck --check-prefix=STATIC %s \
// RUN:   --implicit-check-not=.dll.lib --implicit-check-not=wincrt_dynamic
// STATIC:      "-defaultlib:libc++.lib" "-defaultlib:libunwind.lib"
// STATIC-SAME: "-defaultlib:clang_rt.wincrt_static{{[^"]*}}.lib"

// A DLL's import library is named as the runtimes' are, beside its archive,
// and a DLL embeds no manifest. Under Control Flow Guard, an executable
// suppresses its exports as call targets and a DLL does not; an x86-64 image
// is marked for the shadow stack and has the table of EH continuation targets,
// with or without the guard, all in one -guard: argument.
// RUN: %{x64} -shared -o foo.dll 2>&1 | FileCheck --check-prefixes=DLL,DLL-X64 %s \
// RUN:   --implicit-check-not=-manifest
// RUN: %{cl-a64} /LD /Fefoo.dll /Tc%s 2>&1 | FileCheck --check-prefixes=DLL,DLL-A64 %s \
// RUN:   --implicit-check-not=-manifest --implicit-check-not=-cetcompat
// DLL:          "-out:foo.dll" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"
// DLL-X64-SAME: "-cetcompat" "-guard:cf,ehcont"
// DLL-A64-SAME: "-guard:cf"

// -mwindows and -mconsole select the subsystem, the last one given, and a GUI
// program links user32; -mwindows is accepted when compiling. A user's
// -guard: and -timestamp: replace the toolchain's; SOURCE_DATE_EPOCH leaves
// the timestamp to lld-link.
// RUN: %{x64} -mconsole -mwindows -g -Xlinker -guard:cf,noexportsuppress -Wl,-timestamp:1 2>&1 \
// RUN:   | FileCheck --check-prefix=WINDOWS %s
// RUN: env SOURCE_DATE_EPOCH=1 %{a64} -mwindows -mconsole -mguard=cf-nochecks 2>&1 \
// RUN:   | FileCheck --check-prefix=CONSOLE %s \
// RUN:   --implicit-check-not=user32 --implicit-check-not=-cetcompat --implicit-check-not=-Brepro
// RUN: %{x64} -c -mwindows 2>&1 | FileCheck --check-prefix=COMPILE-ONLY %s
// WINDOWS:      "-lldignoreenv" "-subsystem:windows"
// WINDOWS-SAME: "-manifestinput:{{[^"]*}}" "-debug" "-Brepro" "-cetcompat" "-guard:cf,ehcont,exportsuppress"
// WINDOWS-SAME: "-guard:cf,noexportsuppress" "-timestamp:1"
// WINDOWS-SAME: "-defaultlib:onecore_apiset.lib" "-defaultlib:user32.lib"
// CONSOLE:      "-lldignoreenv" "-subsystem:console"
// CONSOLE-SAME: "-manifestinput:{{[^"]*}}" "-guard:cf,exportsuppress"
// COMPILE-ONLY-NOT: error:

// clang-cl's /guard:cf- leaves the guard out of the link.
// RUN: %{cl-a64} /guard:cf- -- %s 2>&1 | FileCheck --check-prefix=NO-GUARD %s --implicit-check-not=-guard:
// NO-GUARD: lld-link{{(.exe)?}}"

// Without the default libraries an executable still embeds the manifest.
// -rdynamic passes nothing to the linker and is not reported as unused.
// RUN: %{cxx-x64} -nostdlib 2>&1 | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:   --implicit-check-not=-defaultlib: --implicit-check-not=-nodefaultlib:
// RUN: %clangxx -### --target=aarch64-unknown-windows-itanium %s -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:   --implicit-check-not=-defaultlib: --implicit-check-not=-nodefaultlib:
// RUN: %{cxx-x64} -nolibc -rdynamic 2>&1 | FileCheck --check-prefix=NOLIBC %s \
// RUN:   --implicit-check-not=wincrt --implicit-check-not=ucrt.lib \
// RUN:   --implicit-check-not=rdynamic --implicit-check-not='"-export'
// NODEFAULTLIBS:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv"
// NODEFAULTLIBS-SAME: "-manifest:embed"
// NODEFAULTLIBS-SAME: "-manifestinput:{{[^"]*}}segment_heap.manifest"
// NOLIBC:      "-defaultlib:libc++.dll.lib" "-defaultlib:libunwind.dll.lib"
// NOLIBC-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-nodefaultlib:msvcrt"

// -l finds the "lib"-prefixed import library, then archive, in every library
// directory, wherever it is given and those passed to the linker included; it
// leaves any other name, and the file -l: names, to lld-link. -static takes
// the archive. Linker options pass through unchanged, ELF's spellings
// included: lld-link takes the ones it implements.
// RUN: %{x64} -lfoo -lbar -lkernel32 -lbaz.lib -l:qux.a -llate -L%t/lib -Wl,-libpath:%t/late \
// RUN:   -rpath /r1 -Wl,-rpath,/r2,--version-script=a.map -Xlinker -z -Xlinker defs 2>&1 \
// RUN:   | FileCheck --check-prefix=LIBS %s --implicit-check-not=warning:
// RUN: %{a64} -llate -Xlinker /LIBPATH:%t/late 2>&1 | FileCheck --check-prefix=LIBS-XLINKER %s
// RUN: %{a64} -static -lfoo -L%t/lib 2>&1 | FileCheck --check-prefix=LIBS-STATIC %s
// LIBS:      "-libpath:{{[^"]*}}lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libfoo.dll.lib"
// LIBS-SAME: "{{[^"]*}}lib{{/|\\\\}}libbar.lib"
// LIBS-SAME: "kernel32.lib" "baz.lib" "qux.a"
// LIBS-SAME: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"
// LIBS-SAME: "-libpath:{{[^"]*}}late"
// LIBS-SAME: "-rpath" "/r1" "-rpath" "/r2" "--version-script=a.map" "-z" "defs"
// LIBS-XLINKER: "{{[^"]*}}late{{/|\\\\}}liblate.dll.lib"
// LIBS-STATIC:  "{{[^"]*}}lib{{/|\\\\}}libfoo.lib"

// A HIP link takes the HIP runtime from the ROCm installation, and the
// Universal CRT's and the Windows SDK's libraries are found, as with the MSVC
// toolchain. Only lld-link links.
// RUN: %{x64} --hip-link --rocm-path=%S/Inputs/rocm 2>&1 | FileCheck --check-prefix=HIP %s
// RUN: %{x64} -Xmicrosoft-windows-sys-root %t 2>&1 | FileCheck --check-prefix=SDK %s
// RUN: not %{a64} -fuse-ld=bfd 2>&1 | FileCheck --check-prefix=BFD %s
// HIP:      "-libpath:{{[^"]*}}Inputs{{/|\\\\}}rocm{{/|\\\\}}lib" "amdhip64.lib"
// SDK:      "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}ucrt{{/|\\\\}}x64"
// SDK-SAME: "-libpath:[[ROOT]]{{/|\\\\}}Windows Kits{{/|\\\\}}10{{/|\\\\}}Lib{{/|\\\\}}10.0.26100.0{{/|\\\\}}um{{/|\\\\}}x64"
// BFD: error: unsupported option '-fuse-ld=bfd' for target 'aarch64-unknown-windows-itanium'

// The std module manifest is beside libc++'s import library.
// RUN: %clang -print-library-module-manifest-path -stdlib=libc++ \
// RUN:   --target=x86_64-unknown-windows-itanium -resource-dir=%t/lib | FileCheck --check-prefix=MANIFEST %s
// MANIFEST: lib{{/|\\}}libc++.modules.json

//--- lib/libfoo.dll.lib
//--- lib/libfoo.lib
//--- lib/libbar.lib
//--- lib/libc++.dll.lib
//--- lib/libc++.modules.json
//--- late/liblate.dll.lib
//--- Windows Kits/10/Include/10.0.26100.0/ucrt/stdio.h
int puts(const char *);
