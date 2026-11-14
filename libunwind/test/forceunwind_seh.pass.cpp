//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{(x86_64|aarch64).*-windows.*}}
// ADDITIONAL_COMPILE_FLAGS: -fms-extensions

// A forced unwind runs the C++ cleanups and, through a frame that another
// language's handler owns, its termination handlers in that frame, and
// consults none of its exception filters, as an unwind with no target does.

#include <stdlib.h>
#include <unwind.h>

static bool finalized, filtered, cleaned_up;

struct Cleanup { ~Cleanup() { cleaned_up = true; } };

static _Unwind_Reason_Code stop(int, _Unwind_Action actions, _Unwind_Exception_Class, _Unwind_Exception *,
                                struct _Unwind_Context *, void *) {
  if (!(actions & _UA_FORCE_UNWIND) || cleaned_up)
    exit(actions & _UA_FORCE_UNWIND && finalized && !filtered ? 0 : 1);
  return _URC_NO_REASON;
}

__attribute__((noinline)) static void foreign() {
  volatile int local = 42;
  __try {
    __try {
      static _Unwind_Exception ex;
      ex.exception_class = 0x434C4E47554E5700; // "CLNGUNW\0"
      _Unwind_ForcedUnwind(&ex, stop, nullptr);
    } __finally {
      finalized = local == 42;
    }
  } __except (filtered = true, 0) {
  }
}

int main(int, char **) {
  Cleanup c;
  foreign();
  return 1;
}
