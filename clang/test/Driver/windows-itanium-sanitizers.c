// REQUIRES: x86-registered-target

// Test sanitizer options for Windows Itanium toolchain.
// This mirrors mingw-sanitizers.c and MSVC sanitizer tests.

//===----------------------------------------------------------------------===//
// AddressSanitizer - x86_64
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fsanitize=address -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASAN_X64 %s

// ASAN_X64: "-cc1"
// ASAN_X64-SAME: "-fsanitize=address"
// ASAN_X64: lld-link
// ASAN_X64-SAME: "-debug"
// ASAN_X64-SAME: "-incremental:no"
// ASAN_X64: "{{.*}}clang_rt.asan_dynamic.lib"
// ASAN_X64-SAME: "-wholearchive:{{[^"]*}}clang_rt.asan_dynamic_runtime_thunk{{[^"]*}}"
// ASAN_X64-SAME: "-include:__asan_seh_interceptor"

//===----------------------------------------------------------------------===//
// AddressSanitizer - i686 (x86)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=i686-unknown-windows-itanium -fsanitize=address -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASAN_X86 %s

// ASAN_X86: "-cc1"
// ASAN_X86-SAME: "-fsanitize=address"
// ASAN_X86: lld-link
// ASAN_X86-SAME: "-debug"
// ASAN_X86-SAME: "-incremental:no"
// ASAN_X86: "{{.*}}clang_rt.asan_dynamic.lib"
// ASAN_X86-SAME: "-wholearchive:{{[^"]*}}clang_rt.asan_dynamic_runtime_thunk{{[^"]*}}"
// Note: x86 uses triple underscore due to name mangling
// ASAN_X86-SAME: "-include:___asan_seh_interceptor"

//===----------------------------------------------------------------------===//
// AddressSanitizer with pointer checks
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium \
// RUN:   -fsanitize=address,pointer-compare,pointer-subtract -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASAN_POINTER %s

// ASAN_POINTER: "-cc1"
// ASAN_POINTER-SAME: "-fsanitize=address,pointer-compare,pointer-subtract"

//===----------------------------------------------------------------------===//
// Fuzzer
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fsanitize=fuzzer -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUZZER %s

// FUZZER: "-cc1"
// FUZZER-SAME: "-fsanitize=fuzzer,fuzzer-no-link"
// FUZZER: lld-link
// FUZZER-SAME: "-wholearchive:{{[^"]*}}clang_rt.fuzzer{{[^"]*}}"

//===----------------------------------------------------------------------===//
// Fuzzer with -shared (library, no fuzzer main)
//===----------------------------------------------------------------------===//

// RUN: %clang --target=x86_64-unknown-windows-itanium -fsanitize=fuzzer -shared -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=FUZZER_SHARED %s

// FUZZER_SHARED: lld-link
// FUZZER_SHARED-SAME: "-dll"
// FUZZER_SHARED-NOT: "-wholearchive:{{.*}}clang_rt.fuzzer{{.*}}"

//===----------------------------------------------------------------------===//
// Unsupported sanitizers should error
//===----------------------------------------------------------------------===//

// RUN: not %clang --target=x86_64-unknown-windows-itanium -fsanitize=thread -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=TSAN_ERR %s

// TSAN_ERR: error: unsupported option '-fsanitize=thread' for target

// RUN: not %clang --target=x86_64-unknown-windows-itanium -fsanitize=memory -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=MSAN_ERR %s

// MSAN_ERR: error: unsupported option '-fsanitize=memory' for target

//===----------------------------------------------------------------------===//
// AddressSanitizer with ARM64
//===----------------------------------------------------------------------===//

// RUN: %clang --target=aarch64-unknown-windows-itanium -fsanitize=address -### %s 2>&1 \
// RUN:   | FileCheck -check-prefix=ASAN_ARM64 %s

// ASAN_ARM64: "-cc1"
// ASAN_ARM64-SAME: "-fsanitize=address"
// ASAN_ARM64: lld-link
// ASAN_ARM64-SAME: "-debug"
// ASAN_ARM64-SAME: "-incremental:no"
// ASAN_ARM64: "{{.*}}clang_rt.asan_dynamic.lib"
// ASAN_ARM64-SAME: "-include:__asan_seh_interceptor"
