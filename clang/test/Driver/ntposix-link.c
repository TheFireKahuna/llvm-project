// The NT-POSIX toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// The start-up objects, then the inputs, then llvm-libc and the system
// import libraries it is layered on.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=EXE %s \
// RUN:       --implicit-check-not=libc++ \
// RUN:       --implicit-check-not=oldnames \
// RUN:       --implicit-check-not=dllcrt.obj \
// RUN:       --implicit-check-not=-defaultlib:ucrt
// EXE:      lld-link{{(.exe)?}}" "-out:a.exe" "-machine:x64" "-nologo" "-lldignoreenv"
// EXE-SAME: "-manifest:embed"
// EXE-SAME: "[[LIB:[^"]*]]{{/|\\\\}}crt1.obj" "[[LIB]]{{/|\\\\}}crt_do_start.obj"
// EXE-SAME: "[[LIB]]{{/|\\\\}}crt_tls.obj" "[[LIB]]{{/|\\\\}}crt_tls_cleanup.obj"
// EXE-SAME: "[[LIB]]{{/|\\\\}}crt_gs.obj" "[[LIB]]{{/|\\\\}}crt_cfg.obj"
// EXE-SAME: "[[LIB]]{{/|\\\\}}crt_loadcfg.obj"
// EXE-SAME: "{{[^"]*}}.o"
// EXE-SAME: "-defaultlib:libunwind.dll.lib"
// EXE-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// EXE-SAME: "-defaultlib:libc.dll.lib"
// EXE-SAME: "-defaultlib:[[LIB]]{{/|\\\\}}ntdll.lib"
// EXE-SAME: "-defaultlib:[[LIB]]{{/|\\\\}}sspicli.lib"
// EXE-SAME: "-defaultlib:[[LIB]]{{/|\\\\}}bcryptprimitives.lib"
// EXE-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// EXE-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// EXE-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// EXE-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:ucrt"

// A DLL's start-up leaves thread-detach cleanup to libc.dll.
// RUN: %clangxx -### --target=aarch64-pc-windows-ntposix %s --sysroot=%t \
// RUN:     -shared -o foo.dll 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s \
// RUN:       --implicit-check-not=crt1.obj \
// RUN:       --implicit-check-not=crt_do_start.obj \
// RUN:       --implicit-check-not=crt_tls_cleanup.obj
// DLL:      "-machine:arm64"
// DLL-SAME: "-dll" "-implib:foo.dll.lib"
// DLL-SAME: "{{[^"]*}}dllcrt.obj" "{{[^"]*}}crt_tls.obj" "{{[^"]*}}crt_gs.obj"
// DLL-SAME: "{{[^"]*}}crt_cfg.obj" "{{[^"]*}}crt_loadcfg.obj"
// DLL-SAME: "-defaultlib:libc++.dll.lib" "-defaultlib:libunwind.dll.lib"
// DLL-SAME: "-defaultlib:libc.dll.lib"

// -static links libc, libc++ and libunwind statically.
// RUN: %clangxx -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t \
// RUN:     -static 2>&1 \
// RUN:   | FileCheck --check-prefix=STATIC %s \
// RUN:       --implicit-check-not=.dll.lib
// STATIC:      "-defaultlib:{{[^"]*}}libc++.lib"
// STATIC-SAME: "-defaultlib:{{[^"]*}}libunwind.lib"
// STATIC-SAME: "-defaultlib:{{[^"]*}}libc.lib"

// ... or fails when an archive is missing.
// RUN: not %clangxx -### --target=x86_64-pc-windows-ntposix %s \
// RUN:     --sysroot=%t/nostatic -static -nostartfiles 2>&1 \
// RUN:   | FileCheck --check-prefix=STATIC-MISSING %s
// STATIC-MISSING-DAG: error: no such file or directory: 'libc++.lib'
// STATIC-MISSING-DAG: error: no such file or directory: 'libunwind.lib'
// STATIC-MISSING-DAG: error: no such file or directory: 'libc.lib'

// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t \
// RUN:     -nostartfiles 2>&1 \
// RUN:   | FileCheck --check-prefix=NOSTARTFILES %s \
// RUN:       --implicit-check-not=crt1.obj \
// RUN:       --implicit-check-not=crt_tls.obj
// NOSTARTFILES: "-defaultlib:libc.dll.lib"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t \
// RUN:     -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// NODEFAULTLIBS: "{{[^"]*}}crt1.obj"

//--- lib/crt1.obj
//--- lib/crt_do_start.obj
//--- lib/crt_tls.obj
//--- lib/crt_tls_cleanup.obj
//--- lib/crt_gs.obj
//--- lib/crt_cfg.obj
//--- lib/crt_loadcfg.obj
//--- lib/dllcrt.obj
//--- lib/libc.lib
//--- lib/libc++.lib
//--- lib/libunwind.lib
//--- lib/ntdll.lib
//--- lib/sspicli.lib
//--- lib/bcryptprimitives.lib
//--- nostatic/lib/libc.dll.lib
