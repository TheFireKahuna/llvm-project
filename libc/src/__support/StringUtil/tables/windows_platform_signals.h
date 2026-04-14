//===-- Map of signal numbers to strings for Windows -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows Itanium uses POSIX-compatible signal numbers (defined in
// llvm-libc-macros/windows/signal-macros.h), so the full signal table is
// the C standard set + POSIX set + the three extensions (SIGSTKFLT, SIGWINCH,
// SIGPWR) that are defined in our headers but absent from the POSIX table.
//
// Real-time signals (SIGRTMIN..SIGRTMAX) are not in the table — they are
// handled by build_signal_string() in signal_to_string.cpp which produces
// "Real-time signal N" dynamically.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_SIGNALS_H
#define LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_SIGNALS_H

#include "posix_signals.h"
#include "src/__support/macros/config.h"
#include "stdc_signals.h"
#include "windows_extension_signals.h"

namespace LIBC_NAMESPACE_DECL {

LIBC_INLINE_VAR constexpr auto PLATFORM_SIGNALS =
    STDC_SIGNALS + POSIX_SIGNALS + WINDOWS_SIGNALS;

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_SIGNALS_H
