//===--- Windows/Watchdog.inc - Windows Watchdog Implementation -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the generic Windows implementation of the Watchdog class.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_POSIX
// LLVM libc provides alarm() via POSIX-compatible unistd.h.
#include <unistd.h>

namespace llvm {
namespace sys {
Watchdog::Watchdog(unsigned int seconds) { alarm(seconds); }
Watchdog::~Watchdog() { alarm(0); }
} // namespace sys
} // namespace llvm
#else
// TODO: implement.
// Currently this is only used by PrettyStackTrace which is also unimplemented
// on Windows. Roughly, a Windows implementation would use CreateWaitableTimer
// and a second thread to run the TimerAPCProc.

namespace llvm {
namespace sys {
Watchdog::Watchdog(unsigned int seconds) {}
Watchdog::~Watchdog() {}
} // namespace sys
} // namespace llvm
#endif