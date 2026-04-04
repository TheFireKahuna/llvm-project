//===-- Hermetic runtime aggregator for Windows --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The `libc.startup.windows.hermetic_runtime` object library is the ABI
// floor every hermetic Windows exe needs regardless of subsystem under test:
// C++ itanium guard (__cxa_guard_*), exit/atexit + __cxa_finalize, setjmp,
// and the memory-fault VEH filter statically registered into `.libcveh`.
//
// This TU also forwards extern-"C" `exit` to LIBC_NAMESPACE::exit.
// Entrypoint DEPS resolve to their __internal__ variant (mangled only, no
// extern-"C" public alias), so hermetic exes — which link -nolibc and thus
// have no c.dll to fall back on — would otherwise see undefined `exit`.
// Mirrors the `atexit` shim in HermeticTestUtils.cpp.
//
// `exit` is safe to forward through a plain wrapper (it's a tail-call and
// never returns). `setjmp` is NOT: a forwarder's frame would be captured
// instead of the caller's, breaking longjmp. Its extern-"C" alias is
// emitted directly from the naked-asm impl in setjmp_windows.cpp.
//
// Kept sibling to `crt1` (not folded in) so a future DllMain-hosted hermetic
// exe can drop crt1 without losing the ABI floor.
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
[[noreturn]] void exit(int status);
} // namespace LIBC_NAMESPACE_DECL

extern "C" [[noreturn]] void exit(int status) {
  LIBC_NAMESPACE::exit(status);
}
