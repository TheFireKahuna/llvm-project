// The NT-POSIX toolchain: Win32 declaration regions.

// RUN: rm -rf %t && split-file %s %t

// The Windows SDK's include directories follow the C library's and are Win32
// roots, after Windows Itanium's wrappers, which supply the vcruntime headers
// the SDK expects, and a unit that can reach them takes the Microsoft
// extensions.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %t/foo.c \
// RUN:     -Xmicrosoft-windows-sys-root %t 2>&1 \
// RUN:   | FileCheck --check-prefix=SDK %s
// SDK:      "-cc1"
// SDK-DAG:  "-fwin32-header-root=[[SDK:[^"]*]]{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}shared"
// SDK-DAG:  "-fwin32-header-root=[[SDK]]{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}um"
// SDK-DAG:  "-fwin32-header-root=[[WRAP:[^"]*win32_itanium_wrappers]]"
// SDK-DAG:  "-fms-extensions"
// SDK:      "-internal-isystem" "[[WRAP]]"
// SDK-SAME: "-internal-isystem" "[[SDK]]{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}shared"
// SDK-SAME: "-internal-isystem" "[[SDK]]{{/|\\\\}}Include{{/|\\\\}}10.0.26100.0{{/|\\\\}}um"

// Without the SDK, a unit is POSIX alone.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %t/foo.c \
// RUN:     -Xmicrosoft-windows-sys-root %t/none 2>&1 \
// RUN:   | FileCheck --check-prefix=NONE %s \
// RUN:       --implicit-check-not=-fwin32-header-root \
// RUN:       --implicit-check-not=-fms-extensions --implicit-check-not=-mwin32
// NONE: "-cc1"

// -iwin32 adds a Win32 root after the include path.
// RUN: %clang -### --target=x86_64-pc-windows-ntposix -c %t/foo.c \
// RUN:     -Xmicrosoft-windows-sys-root %t/none -iwin32 %t/app 2>&1 \
// RUN:   | FileCheck --check-prefix=IWIN32 %s
// IWIN32-DAG: "-fwin32-header-root=[[APP:[^"]*app]]"
// IWIN32-DAG: "-idirafter" "[[APP]]"
// IWIN32-DAG: "-fms-extensions"

// -mwin32 is the Win32 face, with its 16-bit wchar_t.
// RUN: %clang -### --target=aarch64-pc-windows-ntposix -c %t/foo.c \
// RUN:     -Xmicrosoft-windows-sys-root %t/none -mwin32 2>&1 \
// RUN:   | FileCheck --check-prefix=FACE %s
// FACE-DAG: "-mwin32"
// FACE-DAG: "-fwchar-type=short"
// FACE-DAG: "-fno-signed-wchar"
// FACE-DAG: "-fms-extensions"

#--- Windows Kits/10/Include/10.0.26100.0/shared/basetsd.h
#--- Windows Kits/10/Include/10.0.26100.0/um/windows.h
#--- Windows Kits/10/Lib/10.0.26100.0/um/x64/kernel32.lib
#--- app/app.h
#--- foo.c
int foo;
