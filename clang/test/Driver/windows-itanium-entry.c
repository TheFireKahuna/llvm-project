// REQUIRES: x86-registered-target

// Test entry point handling for Windows Itanium toolchain.
// The toolchain sets appropriate CRT entry points for executables and DLLs.

//===----------------------------------------------------------------------===//
// Default executable entry point
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=EXE_X64 %s

// EXE_X64: lld-link
// EXE_X64-SAME: "-entry:mainCRTStartup"

// RUN: %clang --target=i686-unknown-windows-itanium -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=EXE_X86 %s

// EXE_X86: lld-link
// EXE_X86-SAME: "-entry:mainCRTStartup"

//===----------------------------------------------------------------------===//
// DLL entry point (architecture-dependent decoration)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_X64 %s

// DLL_X64: lld-link
// DLL_X64-SAME: "-entry:_DllMainCRTStartup"
// DLL_X64-NOT: "-entry:_DllMainCRTStartup@12"

// RUN: %clang --target=i686-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_X86 %s

// x86 uses stdcall decoration (@12 for 3 parameters * 4 bytes)
// DLL_X86: lld-link
// DLL_X86-SAME: "-entry:_DllMainCRTStartup@12"

// RUN: %clang --target=aarch64-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_ARM64 %s

// DLL_ARM64: lld-link
// DLL_ARM64-SAME: "-entry:_DllMainCRTStartup"

// RUN: %clang --target=arm-unknown-windows-itanium -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_ARM %s

// DLL_ARM: lld-link
// DLL_ARM-SAME: "-entry:_DllMainCRTStartup"

//===----------------------------------------------------------------------===//
// -nostartfiles suppresses entry point
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -nostartfiles -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTARTFILES %s

// NOSTARTFILES: lld-link
// NOSTARTFILES-NOT: "-entry:mainCRTStartup"

//===----------------------------------------------------------------------===//
// -nostdlib also suppresses entry point
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -nostdlib -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=NOSTDLIB_ENTRY %s

// NOSTDLIB_ENTRY: lld-link
// NOSTDLIB_ENTRY-NOT: "-entry:mainCRTStartup"

//===----------------------------------------------------------------------===//
// DLL with -nostartfiles still sets entry point
// (entry point is separate from startup files for DLLs)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -shared -nostartfiles -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=DLL_NOSTARTFILES %s

// DLL_NOSTARTFILES: lld-link
// DLL_NOSTARTFILES-SAME: "-entry:_DllMainCRTStartup"
