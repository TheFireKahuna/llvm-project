// The NT-POSIX toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// ntposix.dll's import library and the builtins, as default libraries after
// the inputs: no C library, no unwind library, no start files, and no option
// that selects how imports bind.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t 2>&1 \
// RUN:   | FileCheck --check-prefix=EXE %s \
// RUN:       --implicit-check-not=libc++ --implicit-check-not=libunwind \
// RUN:       --implicit-check-not=wincrt --implicit-check-not=oldnames \
// RUN:       --implicit-check-not=-defaultlib:ucrt --implicit-check-not=.obj \
// RUN:       --implicit-check-not=-entry: \
// RUN:       --implicit-check-not=-auto-import \
// RUN:       --implicit-check-not=-runtime-pseudo-reloc \
// RUN:       --implicit-check-not=-import-slots \
// RUN:       --implicit-check-not=-start-stop-symbols \
// RUN:       --implicit-check-not=-boundary-symbols
// EXE:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv"
// EXE-SAME: "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}lib{{/|\\\\}}x86_64-pc-windows-ntposix"
// EXE-SAME: "-libpath:[[ROOT]]{{/|\\\\}}lib"
// EXE-SAME: "{{[^"]*}}.o"
// EXE-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib"
// EXE-SAME: "-defaultlib:ntposix" "-nodefaultlib:ucrt" "-nodefaultlib:oldnames"
// EXE-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// EXE-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// EXE-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// EXE-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:iso_stdio_wide_specifiers"

// A C++ link adds libc++; the unwinder is ntposix.dll's.
// RUN: %clangxx -### --target=aarch64-pc-windows-ntposix %s -shared \
// RUN:     -o foo.dll 2>&1 \
// RUN:   | FileCheck --check-prefix=DLL %s --implicit-check-not=libunwind
// DLL:      "-out:foo.dll" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"
// DLL-SAME: "-defaultlib:libc++.dll.lib"
// DLL-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-defaultlib:ntposix"

// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s -nolibc 2>&1 \
// RUN:   | FileCheck --check-prefix=NOLIBC %s \
// RUN:       --implicit-check-not=-defaultlib:ntposix
// NOLIBC: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-nodefaultlib:msvcrt"

// RUN: %clang -### --target=aarch64-pc-windows-ntposix %s -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s \
// RUN:       --implicit-check-not=-defaultlib: \
// RUN:       --implicit-check-not=-nodefaultlib:
// NODEFAULTLIBS: lld-link{{(.exe)?}}"

//--- lib/x86_64-pc-windows-ntposix/ntposix.lib
//--- lib/libc.lib
