//===-- Env-gated tracepoints for Tier A bootstrap --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Developer-only observability for `__libc_bootstrap()`. Two opt-ins,
// both read directly from `PEB->ProcessParameters->Environment` at Tier
// A time (no env parser, no allocator, no syscall — just a UTF-16 scan
// of a memory block that's already mapped by the loader):
//
//   LIBC_BOOTSTRAP_TRACE=1  — emit one `DbgPrintEx` line per phase boundary.
//                             Output goes to the debugger output stream
//                             (attached debugger, DebugView, WinDbg).
//   LIBC_BOOTSTRAP_BREAK=1  — `__debugbreak()` at the end of Tier A,
//                             right after PCB Zone 0/0b seal. Useful
//                             for attaching a debugger to inspect the
//                             just-sealed PCB state.
//
// Both default to OFF. A missing or unset variable is the same as "=0".
// This header is freestanding-safe — included only from
// libc_bootstrap.cpp, which is already freestanding.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_STARTUP_WINDOWS_TIER_A_TRACE_H
#define LLVM_LIBC_STARTUP_WINDOWS_TIER_A_TRACE_H

#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/nt_debug.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Case-insensitive (ASCII-fold) UTF-16 scan for `NAME=<truthy>` in the
// PEB env block. Matches Windows env semantics — `Path`, `PATH`, and
// `path` are the same variable — so developer knobs behave the way a
// Windows user expects regardless of how the variable was set.
//
// A value is "truthy" iff its first character is one of `1..9`, `y`,
// `Y`, `t`, `T` — which covers `1`, `y`/`Y`, `yes`/`YES`, `t`/`T`,
// `true`/`TRUE` — OR the full value is exactly `on` (case-insensitive),
// which has to be distinguished from `off`/`OFF` explicitly because
// they share the same leading letter. Empty, `0`, `n*`, `f*`, `off` are
// all rejected.
//
// Character type: we use `char16_t`, NOT `wchar_t`. NT's WCHAR is a
// fixed 16-bit UTF-16 code unit, and the PEB env block stores exactly
// that. The NTPOSIX toolchain sets `-fwchar-type=int -fsigned-wchar`
// (32-bit `wchar_t`), so a `wchar_t *` view would read 4-byte strides
// over a 2-byte stream and silently corrupt every comparison. `char16_t`
// is ISO-fixed at 16 bits and matches NT's `WCHAR` typedef
// (`using WCHAR = char16_t` in nt_types.h / sys/ntabi.h).
//
// `name` is a plain ASCII NUL-terminated string; each byte is zero-
// extended to 16 bits for the comparison. The env block itself is
// `WCHAR` double-null-terminated (`NAME=VAL\0NAME=VAL\0...\0\0`), but
// we do **not** rely on that terminator: the scan is hard-bounded by
// `RTL_USER_PROCESS_PARAMETERS::EnvironmentSize` (bytes). If a caller
// has corrupted or never populated the block, the worst case is that
// we read up to `EnvironmentSize` bytes of already-committed memory
// and return false, rather than walking off the end.
//
// Threading: Tier A runs before any libc-owned thread exists and before
// any code that might call `SetEnvironmentVariableW`, so the block is
// quiescent during this scan. Do not call this after Tier B.
LIBC_INLINE bool tier_a_env_flag_set(cpp::string_view name) {
  // ASCII fold to upper-case; pass non-ASCII (≥0x80) through unchanged.
  auto fold = [](unsigned c) -> unsigned {
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
  };
  auto name_matches_ci = [&fold](cpp::string_view n,
                                  cpp::w16string_view head) -> bool {
    if (n.size() != head.size())
      return false;
    for (size_t i = 0; i < n.size(); ++i)
      if (fold(static_cast<unsigned>(head[i])) !=
          fold(static_cast<unsigned char>(n[i])))
        return false;
    return true;
  };
  auto value_is_truthy = [](cpp::w16string_view v) -> bool {
    if (v.empty())
      return false;
    char16_t c0 = v.front();
    if (c0 >= u'1' && c0 <= u'9')
      return true;
    if (c0 == u'y' || c0 == u'Y' || c0 == u't' || c0 == u'T')
      return true;
    // `on`/`ON`: exactly two letters. Explicitly distinguished from
    // `off`/`OFF` which share the same leading letter.
    if (v.size() == 2 && (c0 == u'o' || c0 == u'O')) {
      char16_t c1 = v[1];
      if (c1 == u'n' || c1 == u'N')
        return true;
    }
    return false;
  };

  const PEB *peb = NtCurrentPeb();
  if (!peb || !peb->ProcessParameters)
    return false;
  const RTL_USER_PROCESS_PARAMETERS *pp = peb->ProcessParameters;
  const char16_t *env_data = static_cast<const char16_t *>(pp->Environment);
  if (!env_data)
    return false;
  // `EnvironmentSize` is in bytes and always set by the Windows loader
  // on supported releases (Win11 22631+). A zero here means an unusual
  // launch path (e.g. a non-normalised RTL_USER_PROCESS_PARAMETERS from
  // a hand-rolled NtCreateUserProcess caller); decline rather than
  // fall back to an unbounded scan.
  size_t env_bytes = static_cast<size_t>(pp->EnvironmentSize);
  if (env_bytes < sizeof(char16_t))
    return false;
  // Sanity cap against a garbage `EnvironmentSize`. CreateProcess docs
  // cap the env block at 32767 chars; real programs stay well under a
  // megabyte. 16 MiB is a generous ceiling that still prevents pointer
  // arithmetic from wrapping the address space below.
  constexpr size_t MAX_ENV_BYTES = size_t{16} << 20;
  if (env_bytes > MAX_ENV_BYTES)
    env_bytes = MAX_ENV_BYTES;

  // `block` is the remaining, yet-to-scan slice of the env region. The
  // view's length is the hard bound — every read below stays inside it
  // by construction, so there are no ad-hoc pointer comparisons.
  cpp::w16string_view block(env_data, env_bytes / sizeof(char16_t));

  while (!block.empty() && block.front() != u'\0') {
    // Carve out the current `NAME=VALUE` entry, bounded by the block.
    size_t entry_len = 0;
    while (entry_len < block.size() && block[entry_len] != u'\0')
      ++entry_len;
    cpp::w16string_view entry = block.substr(0, entry_len);

    // Split on the first `=`. Entries without `=` (e.g. malformed) are
    // skipped.
    size_t eq = 0;
    while (eq < entry.size() && entry[eq] != u'=')
      ++eq;
    if (eq < entry.size() &&
        name_matches_ci(name, entry.substr(0, eq)) &&
        value_is_truthy(entry.substr(eq + 1)))
      return true;

    // Advance past the entry and its terminating NUL (if present).
    block.remove_prefix(entry_len);
    if (!block.empty())
      block.remove_prefix(1);
  }
  return false;
}

struct TierATraceState {
  bool trace_on;
  bool break_on;

  LIBC_INLINE static TierATraceState load() {
    return TierATraceState{tier_a_env_flag_set("LIBC_BOOTSTRAP_TRACE"),
                           tier_a_env_flag_set("LIBC_BOOTSTRAP_BREAK")};
  }

  LIBC_INLINE void phase(const char *name) const {
    if (trace_on)
      ::DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[libc] Tier A: %s\n", name);
  }

  LIBC_INLINE void end_of_tier_a() const {
    if (trace_on)
      ::DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "[libc] Tier A: complete\n");
    if (break_on)
      __builtin_debugtrap();
  }
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_STARTUP_WINDOWS_TIER_A_TRACE_H
