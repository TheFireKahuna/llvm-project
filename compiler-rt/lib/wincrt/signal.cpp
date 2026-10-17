//===-- signal.cpp - Default signal actions and abort ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// raise, which every image's entry object wraps with this definition. The
// Universal CRT's raise takes a signal's default action, which for every
// signal it supports is to end the process with code 3, by calling its own
// _exit. The wrap of _exit cannot reach that call, and the Universal CRT's
// _exit loads kernel.appcore.dll and msvcrt.dll to read the app model's
// termination policy. This raise ends the process through _exit, which the
// wrap makes wincrt's, when the action is the default, and leaves every other
// case to the Universal CRT's: SIG_IGN, the handler call and the reset before
// it, and the error for a signal it does not support.
//
// abort is wrapped for the same reason: the Universal CRT's ends the process
// through its own _exit when the program has cleared _CALL_REPORTFAULT.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <signal.h>
#include <stdlib.h>

extern "C" {

// The Universal CRT's raise, which the wrap binds to this name. The wrap
// renames the symbol and not its import pointer, so the call is direct.
__attribute__((visibility("hidden"))) int __cdecl __real_raise(int);

WINCRT_ATEXIT_API int __cdecl __wrap_raise(int Signal) {
  switch (Signal) {
  case SIGINT:
  case SIGILL:
  case SIGABRT_COMPAT:
  case SIGFPE:
  case SIGSEGV:
  case SIGTERM:
  case SIGBREAK:
  case SIGABRT:
    // SIG_GET reads the action as the Universal CRT's raise does, under its
    // lock for the process-wide signals, so a default action read here is
    // the one that raise would have taken at this point. Its side effects,
    // the console control handler for SIGINT and SIGBREAK and the thread's
    // own copy of the exception-action table for the others, arise only where
    // no handler was ever set, where the action is the default.
    if (signal(Signal, SIG_GET) == SIG_DFL)
      _exit(3);
    break;
  }
  return __real_raise(Signal);
}

// abort as the Universal CRT's release build does it: a SIGABRT handler runs
// first, through raise; then the process fails fast if _CALL_REPORTFAULT is
// set, and otherwise ends with code 3. Only the debug build acts on
// _WRITE_ABORT_MSG, so there is no message to write.
WINCRT_ATEXIT_API [[noreturn]] void __cdecl __wrap_abort(void) {
  if (signal(SIGABRT, SIG_GET) != SIG_DFL)
    raise(SIGABRT);
  // The flags are read after the handler, which may change them. An empty
  // mask reads them and stores them back unchanged.
  if (_set_abort_behavior(0, 0) & _CALL_REPORTFAULT)
    // Every supported Windows fails fast, so the Universal CRT's fallback to
    // its own fault report is never taken.
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  _exit(3);
}

} // extern "C"
