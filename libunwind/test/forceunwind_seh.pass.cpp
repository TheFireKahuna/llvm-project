// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.*-windows.*}}

// A forced unwind on SEH runs each frame's cleanups and reaches the end of the
// stack, calling every frame's language handler on the way, including
// handlers that are not valid indirect-call targets, such as ntdll's
// __C_specific_handler in the thread's initial frame, which Control Flow
// Guard would reject.

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unwind.h>

static bool cleaned_up = false;

struct Cleanup {
  ~Cleanup() { cleaned_up = true; }
};

static _Unwind_Reason_Code stop(int version, _Unwind_Action actions,
                                _Unwind_Exception_Class, _Unwind_Exception *,
                                struct _Unwind_Context *, void *) {
  assert(version == 1);
  assert((actions & _UA_FORCE_UNWIND) != 0);
  if (actions & _UA_END_OF_STACK) {
    assert(cleaned_up);
    exit(0);
  }
  return _URC_NO_REASON;
}

__attribute__((noinline)) static void unwind() {
  static _Unwind_Exception ex;
  ex.exception_class = 0x434C4E47554E5700; // "CLNGUNW\0"
  _Unwind_ForcedUnwind(&ex, stop, nullptr);
}

__attribute__((noinline)) static void with_cleanup() {
  Cleanup c;
  unwind();
}

int main(int, char **) {
  with_cleanup();
  return 1;
}
