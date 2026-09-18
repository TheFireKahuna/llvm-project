//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.*-windows-itanium.*}}

// On Windows Itanium, longjmp performs a full RtlUnwindEx through every
// skipped frame. Frames with C++ cleanups carry the Itanium SEH personality
// (_GCC_specific_handler); it must let the STATUS_LONGJUMP forced unwind pass
// without running cleanups. Running them is impossible to do correctly (the
// longjmp value travels in RtlUnwindEx's ReturnValue register, invisible to
// frame handlers, and RtlRestoreContext special-cases STATUS_LONGJUMP
// records), and attempting a collided unwind into the cleanup landing pad
// used to re-enter the same handler until stack overflow.

#include <assert.h>
#include <setjmp.h>

static jmp_buf jb;
static int dtor_ran = 0;

struct Guard {
  ~Guard() { ++dtor_ran; }
};

__attribute__((noinline)) static void jumper() {
  Guard g;
  longjmp(jb, 42);
}

int main(int, char **) {
  int v = setjmp(jb);
  if (v == 0) {
    jumper();
    return 1;
  }
  // The value must arrive intact and the skipped frame's destructor must not
  // have run (longjmp over C++ frames matches plain-C / MinGW semantics).
  assert(v == 42);
  assert(dtor_ran == 0);
  return 0;
}
