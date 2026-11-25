// The NT-POSIX toolchain: linking.

// RUN: rm -rf %t && split-file %s %t

// ntposix.dll's import library and the builtins, as default libraries after
// the inputs, with the builtins' load configuration: no C library, no unwind
// library, no start files, and no option that selects how imports bind. An
// executable embeds a manifest, and an image's timestamps are a hash of its
// contents.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s --sysroot=%t -resource-dir=%t/resource 2>&1 | FileCheck --check-prefix=EXE %s \
// RUN:   --implicit-check-not=libc++ --implicit-check-not=libunwind --implicit-check-not=wincrt \
// RUN:   --implicit-check-not=oldnames --implicit-check-not=-defaultlib:ucrt --implicit-check-not=.obj \
// RUN:   --implicit-check-not=-entry: --implicit-check-not=-manifestinput: --implicit-check-not=-auto-import \
// RUN:   --implicit-check-not=-runtime-pseudo-reloc --implicit-check-not=-import-slots \
// RUN:   --implicit-check-not=-start-stop-symbols --implicit-check-not=-boundary-symbols
// EXE:      lld-link{{(.exe)?}}" "-out:a.exe" "-lldignoreenv" "-manifest:embed" "-Brepro"
// EXE-SAME: "-cetcompat" "-guard:ehcont,nocf"
// EXE-SAME: "-libpath:[[ROOT:[^"]*]]{{/|\\\\}}lib{{/|\\\\}}x86_64-pc-windows-ntposix"
// EXE-SAME: "-libpath:[[ROOT]]{{/|\\\\}}lib"
// EXE-SAME: "{{[^"]*}}.o"
// EXE-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-include:_load_config_used"
// EXE-SAME: "-defaultlib:ntposix" "-nodefaultlib:ucrt" "-nodefaultlib:oldnames"
// EXE-SAME: "-nodefaultlib:msvcrt" "-nodefaultlib:msvcrtd"
// EXE-SAME: "-nodefaultlib:vcruntime" "-nodefaultlib:vcruntimed"
// EXE-SAME: "-nodefaultlib:libcmt" "-nodefaultlib:libcmtd"
// EXE-SAME: "-nodefaultlib:ucrtd" "-nodefaultlib:iso_stdio_wide_specifiers"

// A C++ link adds libc++; the unwinder is ntposix.dll's.
// RUN: %clangxx -### --target=aarch64-pc-windows-ntposix %s -shared -o foo.dll 2>&1 | FileCheck --check-prefix=DLL %s \
// RUN:   --implicit-check-not=libunwind --implicit-check-not=-manifest --implicit-check-not=-cetcompat
// DLL:      "-out:foo.dll" "-lldignoreenv" "-dll" "-implib:foo.dll.lib"
// DLL-SAME: "-guard:ehcont,nocf"
// DLL-SAME: "-defaultlib:libc++.dll.lib"
// DLL-SAME: "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-include:_load_config_used" "-defaultlib:ntposix"

// Every image has the table of EH continuation targets, which ntposix.dll's
// unwinder is checked against, and an x86-64 image is marked for the shadow
// stack; Control Flow Guard is not yet a default. Without the default
// libraries an executable still embeds a manifest, and names no load
// configuration, nor does a compilation.
// RUN: %clang -### --target=aarch64-pc-windows-ntposix %s -mguard=cf -nodefaultlibs 2>&1 \
// RUN:   | FileCheck --check-prefix=NODEFAULTLIBS %s --implicit-check-not=-defaultlib: --implicit-check-not=-nodefaultlib: \
// RUN:   --implicit-check-not=_load_config_used
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s -c 2>&1 | FileCheck --check-prefix=COMPILE %s \
// RUN:   --implicit-check-not=_load_config_used
// RUN: %clang -### --target=x86_64-pc-windows-ntposix %s -nolibc 2>&1 | FileCheck --check-prefix=NOLIBC %s \
// RUN:   --implicit-check-not=-defaultlib:ntposix
// NODEFAULTLIBS: "-manifest:embed" "-Brepro" "-guard:cf,ehcont,exportsuppress"
// NOLIBC:        "-defaultlib:clang_rt.builtins{{[^"]*}}.lib" "-include:_load_config_used" "-nodefaultlib:msvcrt"
// COMPILE:       "-cc1"

//--- lib/x86_64-pc-windows-ntposix/ntposix.lib
//--- lib/libc.lib
