//===-- Unittests for the public errno.h contract -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <errno.h>

#include "test/UnitTest/Test.h"

#if defined(__NTPOSIX__)

#ifndef EBADMSG
#error "NTPOSIX errno.h must provide EBADMSG"
#endif

#ifndef EDQUOT
#error "NTPOSIX errno.h must provide EDQUOT"
#endif

#ifndef EMULTIHOP
#error "NTPOSIX errno.h must provide EMULTIHOP"
#endif

#ifndef ENODATA
#error "NTPOSIX errno.h must provide ENODATA"
#endif

#ifndef ENOSR
#error "NTPOSIX errno.h must provide ENOSR"
#endif

#ifndef ENOSTR
#error "NTPOSIX errno.h must provide ENOSTR"
#endif

#ifndef ESTALE
#error "NTPOSIX errno.h must provide ESTALE"
#endif

#ifndef ETIME
#error "NTPOSIX errno.h must provide ETIME"
#endif

#ifndef EDEADLOCK
#error "NTPOSIX errno.h must provide EDEADLOCK"
#endif

#ifndef EWOULDBLOCK
#error "NTPOSIX errno.h must provide EWOULDBLOCK"
#endif

static_assert(ENOSTR == 60);
static_assert(ENODATA == 61);
static_assert(ETIME == 62);
static_assert(ENOSR == 63);
static_assert(EMULTIHOP == 72);
static_assert(EBADMSG == 74);
static_assert(ESTALE == 116);
static_assert(EDQUOT == 122);

static_assert(ENOTSUP == EOPNOTSUPP);
static_assert(EDEADLOCK == EDEADLK);
static_assert(EWOULDBLOCK == EAGAIN);

#endif // __NTPOSIX__

TEST(LlvmLibcErrnoHeaderTest, ErrnoRoundTrip) {
  errno = 0;
  ASSERT_EQ(errno, 0);

  errno = ERANGE;
  ASSERT_EQ(errno, ERANGE);

  int *errno_ptr = __llvm_libc_errno();
  ASSERT_NE(errno_ptr, nullptr);
  ASSERT_EQ(*errno_ptr, ERANGE);

  *errno_ptr = EDOM;
  ASSERT_EQ(errno, EDOM);
}
