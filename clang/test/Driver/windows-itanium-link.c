// REQUIRES: x86-registered-target

// Test linker options for Windows Itanium toolchain.
// This provides comprehensive coverage of linker flag handling.

//===----------------------------------------------------------------------===//
// Default linker is LLD
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DEFAULT_LINKER %s

// DEFAULT_LINKER: lld-link
// DEFAULT_LINKER-SAME: "-auto-import"
// DEFAULT_LINKER-SAME: "-incremental:no"
// DEFAULT_LINKER-SAME: "-nologo"

//===----------------------------------------------------------------------===//
// -fuse-ld=lld is accepted
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUSE_LD_LLD %s

// FUSE_LD_LLD: lld-link
// FUSE_LD_LLD-NOT: warning:

//===----------------------------------------------------------------------===//
// -fuse-ld=lld-link is accepted
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld-link -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUSE_LD_LLDLINK %s

// FUSE_LD_LLDLINK: lld-link
// FUSE_LD_LLDLINK-NOT: warning:

// For -fuse-ld=link warning test, see windows-itanium-errors.c (LINK_EXE_WARN)

//===----------------------------------------------------------------------===//
// Library search paths (-L)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -L/path/to/libs -L/another/path \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=LIBPATH %s

// LIBPATH: lld-link
// LIBPATH-SAME: "-libpath:/path/to/libs"
// LIBPATH-SAME: "-libpath:/another/path"

//===----------------------------------------------------------------------===//
// Library linking (-l)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -lmylib -lfoo.lib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBS %s

// LIBS: lld-link
// -l without .lib extension gets .lib added
// LIBS-SAME: "mylib.lib"
// -l with .lib extension is passed through
// LIBS-SAME: "foo.lib"

//===----------------------------------------------------------------------===//
// Output file (-o)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -o myapp.exe -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OUTPUT %s

// OUTPUT: lld-link
// OUTPUT-SAME: "-out:myapp.exe"

//===----------------------------------------------------------------------===//
// DLL output and import library
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -o mylib.dll -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_OUTPUT %s

// DLL_OUTPUT: lld-link
// DLL_OUTPUT-SAME: "-out:mylib.dll"
// DLL_OUTPUT-SAME: "-dll"
// DLL_OUTPUT-SAME: "-implib:mylib.lib"

//===----------------------------------------------------------------------===//
// Pass-through linker options via -Wl,
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DEBUG -Wl,/LTCG -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_PASSTHROUGH %s

// LINK_PASSTHROUGH: lld-link
// LINK_PASSTHROUGH-SAME: "/DEBUG"
// LINK_PASSTHROUGH-SAME: "/LTCG"

//===----------------------------------------------------------------------===//
// Object file inputs
//===----------------------------------------------------------------------===//

// RUN: touch %t.obj
// RUN: %clang --target=x86_64-unknown-windows-itanium %t.obj -### 2>&1 \
// RUN:   | FileCheck -check-prefix=OBJ_INPUT %s

// OBJ_INPUT: lld-link
// OBJ_INPUT-SAME: "{{.*}}.obj"

//===----------------------------------------------------------------------===//
// Multiple architectures
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_X86 %s

// LINK_X86: lld-link
// LINK_X86-SAME: "-machine:x86"

// RUN: %clang --target=arm-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_ARM %s

// LINK_ARM: lld-link
// LINK_ARM-SAME: "-machine:arm"

//===----------------------------------------------------------------------===//
// Auto-import is always enabled (required for Windows Itanium)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=AUTO_IMPORT %s

// AUTO_IMPORT: lld-link
// AUTO_IMPORT-SAME: "-auto-import"

//===----------------------------------------------------------------------===//
// Incremental linking is disabled (incompatible with auto-import)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NO_INCREMENTAL %s

// NO_INCREMENTAL: lld-link
// NO_INCREMENTAL-SAME: "-incremental:no"
