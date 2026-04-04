//===-- NT-POSIX test-support shims --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shims specific to the NT-POSIX target whose extern-C aliases aren't emitted
// by the __internal__ entrypoint variants (LIBC_COPT_PUBLIC_PACKAGING off).
//
// Kept in its own TU — not folded into HermeticTestUtils.cpp — so that the
// static archive extracts this obj only when a test actually references one
// of these symbols. Folding the shims in would force every hermetic test to
// resolve the entrypoints they forward to, pulling large swaths of the NT
// personality (e.g. the syscall dispatch table's transitive internal:: set)
// into tests that never call them.
//
//===----------------------------------------------------------------------===//

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

// Each NT-POSIX extern-C shim must live in its own TU — see the sibling
// HermeticTestUtilsNTPOSIX*.cpp files. Putting several shims in one
// object forces lld-link to resolve every shim's forwardee whenever a
// test references any one of them (archive extraction is per-obj, symbol
// resolution is per-obj too). Keeping them separate means a test that
// only uses syscall() doesn't have to declare libc.src.unistd.close /
// getpid as DEPs.
//
// This source file intentionally holds no shims.

namespace LIBC_NAMESPACE_DECL {
namespace {
// Anchor so the TU isn't entirely empty — some tool-chains dislike
// object files with no external/static definitions.
[[maybe_unused]] int libc_hermetic_ntposix_anchor;
} // namespace
} // namespace LIBC_NAMESPACE_DECL
