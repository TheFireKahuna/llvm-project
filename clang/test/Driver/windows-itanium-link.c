// REQUIRES: x86-registered-target

// Linker invocation for the Windows Itanium toolchain.

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefixes=DEFAULT_LINKER,NO_RELOCATOR %s
// DEFAULT_LINKER: lld-link
// DEFAULT_LINKER-SAME: "-nologo"
// DEFAULT_LINKER-SAME: "-auto-import"
// DEFAULT_LINKER-SAME: "-lldignoreenv"
// DEFAULT_LINKER-NOT: link.exe"
// There is no startup relocator; a data reference that would need one is a
// link error.
// NO_RELOCATOR-NOT: "-runtime-pseudo-reloc"

// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUSE_LD_LLD %s
// FUSE_LD_LLD-NOT: warning:
// FUSE_LD_LLD: lld-link
// RUN: %clang --target=x86_64-unknown-windows-itanium -fuse-ld=lld-link -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUSE_LD_LLDLINK %s
// FUSE_LD_LLDLINK-NOT: warning:
// FUSE_LD_LLDLINK: lld-link

// RUN: %clang --target=x86_64-unknown-windows-itanium -L/path/to/libs -L/another/path \
// RUN:   -### %s 2>&1 | FileCheck -check-prefix=LIBPATH %s
// LIBPATH: lld-link
// LIBPATH-SAME: "-libpath:/path/to/libs"
// LIBPATH-SAME: "-libpath:/another/path"

// RUN: %clang --target=x86_64-unknown-windows-itanium -lmylib -lfoo.lib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LIBS %s
// LIBS: lld-link
// LIBS-SAME: "mylib.lib"
// LIBS-SAME: "foo.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -o myapp.exe -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=OUTPUT %s
// OUTPUT: lld-link
// OUTPUT-SAME: "-out:myapp.exe"

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -o mylib.dll -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_OUTPUT %s
// DLL_OUTPUT: lld-link
// DLL_OUTPUT-SAME: "-out:mylib.dll"
// DLL_OUTPUT-SAME: "-dll"
// DLL_OUTPUT-SAME: "-implib:mylib.lib"

// RUN: %clang --target=x86_64-unknown-windows-itanium -Wl,/DEBUG -Wl,/LTCG -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_PASSTHROUGH %s
// LINK_PASSTHROUGH: lld-link
// LINK_PASSTHROUGH-SAME: "/DEBUG"
// LINK_PASSTHROUGH-SAME: "/LTCG"

// RUN: touch %t.obj
// RUN: %clang --target=x86_64-unknown-windows-itanium %t.obj -### 2>&1 \
// RUN:   | FileCheck -check-prefix=OBJ_INPUT %s
// OBJ_INPUT: lld-link
// OBJ_INPUT-SAME: "{{.*}}.obj"

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=LINK_X86 %s
// LINK_X86: lld-link
// LINK_X86-SAME: "-machine:x86"
