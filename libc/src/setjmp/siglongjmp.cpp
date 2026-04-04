//===-- Implementation of siglongjmp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/setjmp/siglongjmp.h"
#include "src/__support/common.h"
#include "src/__support/macros/properties/os.h"
#include "src/setjmp/longjmp.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "hdr/signal_macros.h"
#include "src/signal/sigprocmask.h"
#endif

namespace LIBC_NAMESPACE_DECL {

// On non-Windows: siglongjmp is just longjmp. The signal mask restore is
// handled by the sigsetjmp epilogue at the return site.
//
// On Windows: siglongjmp restores the signal mask HERE (before longjmp),
// because there is no epilogue — sigsetjmp tail-calls setjmp directly so
// the caller's context is captured without an intermediate frame. The mask
// was saved into buf->sigmask by sigsetjmp; did_save_mask indicates whether
// it was saved. SS_ONSTACK is cleared by longjmp itself (see longjmp.cpp).
LLVM_LIBC_FUNCTION(void, siglongjmp, (jmp_buf buf, int val)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  auto *b = reinterpret_cast<__jmp_buf *>(buf);

  // Restore signal mask if sigsetjmp saved it
  if (b->did_save_mask)
    LIBC_NAMESPACE::sigprocmask(SIG_SETMASK, &b->sigmask, nullptr);
#endif
  return LIBC_NAMESPACE::longjmp(buf, val);
}

} // namespace LIBC_NAMESPACE_DECL
