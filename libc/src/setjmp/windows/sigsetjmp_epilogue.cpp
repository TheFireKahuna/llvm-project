//===-- Windows implementation of sigsetjmp_epilogue ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/setjmp/sigsetjmp_epilogue.h"
#include "src/__support/common.h"
#include "src/signal/sigprocmask.h"

#include "hdr/signal_macros.h"

namespace LIBC_NAMESPACE_DECL {

// Called from sigsetjmp after setjmp returns.
//   retval == 0: initial call — save current signal mask into buffer.
//   retval != 0: returning via siglongjmp — restore saved mask.
// On Windows, the epilogue is not used — signal mask restore happens in
// siglongjmp instead. This stub exists to satisfy the CMake dependency
// from the shared sigsetjmp build infrastructure.
[[gnu::returns_twice]] int sigsetjmp_epilogue(sigjmp_buf, int retval) {
  return retval;
}

} // namespace LIBC_NAMESPACE_DECL
