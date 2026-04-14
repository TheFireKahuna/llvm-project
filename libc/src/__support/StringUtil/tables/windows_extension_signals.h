//===-- Map of Windows extension signal numbers to strings ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Windows Itanium uses POSIX-compatible signal numbers (not UCRT's), so the
// extension set is identical to Linux's: SIGSTKFLT, SIGWINCH, SIGPWR. These
// are defined in our signal-macros.h but absent from the POSIX signal table.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_EXTENSION_SIGNALS_H
#define LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_EXTENSION_SIGNALS_H

#include "src/__support/StringUtil/message_mapper.h"
#include "src/__support/macros/config.h"

#include <signal.h>

namespace LIBC_NAMESPACE_DECL {

LIBC_INLINE_VAR constexpr const MsgTable<3> WINDOWS_SIGNALS = {
    MsgMapping(SIGSTKFLT, "Stack fault"),
    MsgMapping(SIGWINCH, "Window changed"),
    MsgMapping(SIGPWR, "Power failure"),
};

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_EXTENSION_SIGNALS_H
