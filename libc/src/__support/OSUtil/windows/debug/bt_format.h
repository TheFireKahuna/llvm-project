//===-- Backtrace formatting utilities ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Async-signal-safe formatting helpers shared by backtrace.cpp and
// crash_handler.cpp.
//
// All functions are out-of-line (defined in bt_format.cpp) except the
// SymbolInfo struct. Heavy logic — PE export walks, PEB Ldr walks,
// NtWriteFile loops — lives in the .cpp to avoid text duplication.
//
// Properties:
//   - No heap allocation.
//   - No libc string functions (uses cpp::StringStream / IntegerToString).
//   - No locks, no syscalls (except NtWriteFile in write_to_handle).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BT_FORMAT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BT_FORMAT_H

#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace bt_fmt {

/// Result of a nearest-symbol lookup in a PE export table.
struct SymbolInfo {
  cpp::string_view name;   // Empty if no symbol found.
  void *addr = nullptr;    // Address of the matched export, or nullptr.
  LIBC_INLINE explicit operator bool() const { return addr != nullptr; }
};

/// Signal-safe write to an NT handle. Loops on partial writes.
void write_to_handle(HANDLE h, cpp::string_view data);

/// Find the nearest named PE export at or below \p addr in module \p base.
SymbolInfo find_nearest_symbol(void *base, const void *addr);

/// Write module basename for a given image base into \p out.
/// Returns chars written. Walks the PEB Ldr list.
int find_module_name(void *base, char *out, size_t outlen);

/// Format one backtrace frame into \p buf.
/// Format: "module(symbol+0xoffset) [0xaddr]"
///     or: "module(+0xoffset) [0xaddr]"
///     or: "???(+0xaddr) [0xaddr]"
/// Returns chars written (not counting NUL). Requires buflen >= 64.
int format_frame(void *addr, char *buf, size_t buflen);

} // namespace bt_fmt
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BT_FORMAT_H
