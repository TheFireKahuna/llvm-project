//===-- Implementation of mainCRTStartup for x86_64 Windows ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/attributes.h"
#include "startup/windows/do_start.h"

// Entry point variants — lld-link picks one of mainCRTStartup,
// wmainCRTStartup, WinMainCRTStartup, wWinMainCRTStartup based on
// /subsystem and which user-side entry (main/wmain/WinMain/wWinMain)
// is present. We define all four as identical thin shims into
// __libc_do_start so any combination links cleanly. argv comes from
// the PEB inside __libc_do_start; the wide variants don't need a
// separate UTF-16 path — __libc_init reads CommandLine and converts
// once for both narrow and wide user entry points.

extern "C" [[noreturn]] void mainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void wmainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void WinMainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void wWinMainCRTStartup() { __libc_do_start(); }
