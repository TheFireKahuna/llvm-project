//===-- Implementation of mainCRTStartup for AArch64 Windows --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/macros/attributes.h"
#include "startup/windows/do_start.h"

// All four /entry: variants — see x86_64/start.cpp for rationale.
extern "C" [[noreturn]] void mainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void wmainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void WinMainCRTStartup() { __libc_do_start(); }
extern "C" [[noreturn]] void wWinMainCRTStartup() { __libc_do_start(); }
