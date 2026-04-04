//===-- Unittests for the UTF-8-only locale codeset profile --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// NT-POSIX runs a UTF-8-only locale profile: setlocale/newlocale reject any
// explicit non-UTF-8 codeset suffix so callers never observe silent UTF-8
// substitution. These tests pin the accept/reject boundary.
//
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/locale_macros.h"
#include "include/llvm-libc-macros/langinfo-macros.h"
#include "src/__support/libc_errno.h"
#include "src/locale/freelocale.h"
#include "src/locale/newlocale.h"
#include "src/locale/nl_langinfo.h"
#include "src/locale/setlocale.h"
#include "test/UnitTest/Test.h"

TEST(LlvmLibcLocaleCodeset, CLocaleAccepted) {
  EXPECT_NE(LIBC_NAMESPACE::setlocale(LC_ALL, "C"),
            static_cast<char *>(nullptr));
  EXPECT_NE(LIBC_NAMESPACE::setlocale(LC_ALL, "POSIX"),
            static_cast<char *>(nullptr));
}

TEST(LlvmLibcLocaleCodeset, CLocaleCodesetIsUtf8) {
  // NT-POSIX is a UTF-8-only locale profile (see locale.h): the libc has a
  // single multibyte codec, and CODESET advertises that uniformly across C,
  // POSIX, and every accepted NLS locale rather than lying about a single-
  // byte path that no conversion routine actually implements.
  LIBC_NAMESPACE::setlocale(LC_ALL, "C");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo(CODESET), "UTF-8");
}

#if defined(_WIN32) || defined(_WIN32_ITANIUM)

TEST(LlvmLibcLocaleCodeset, BareNameAccepted) {
  locale_t loc = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US", nullptr);
  ASSERT_NE(loc, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(loc);
}

TEST(LlvmLibcLocaleCodeset, Utf8SuffixAccepted) {
  locale_t a = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US.UTF-8", nullptr);
  ASSERT_NE(a, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(a);

  locale_t b = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US.utf8", nullptr);
  ASSERT_NE(b, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(b);

  locale_t c = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US.UTF8", nullptr);
  ASSERT_NE(c, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(c);

  locale_t d = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US.utf-8", nullptr);
  ASSERT_NE(d, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(d);
}

TEST(LlvmLibcLocaleCodeset, ModifierAccepted) {
  locale_t loc =
      LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "de-DE.UTF-8@euro", nullptr);
  ASSERT_NE(loc, static_cast<locale_t>(nullptr));
  LIBC_NAMESPACE::freelocale(loc);
}

TEST(LlvmLibcLocaleCodeset, SjisRejected) {
  libc_errno = 0;
  locale_t loc = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "ja-JP.SJIS", nullptr);
  EXPECT_EQ(loc, static_cast<locale_t>(nullptr));
  EXPECT_EQ(static_cast<int>(libc_errno), EINVAL);
}

TEST(LlvmLibcLocaleCodeset, Iso88591Rejected) {
  libc_errno = 0;
  locale_t loc =
      LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "en-US.ISO-8859-1", nullptr);
  EXPECT_EQ(loc, static_cast<locale_t>(nullptr));
  EXPECT_EQ(static_cast<int>(libc_errno), EINVAL);
}

TEST(LlvmLibcLocaleCodeset, GbkRejected) {
  libc_errno = 0;
  locale_t loc = LIBC_NAMESPACE::newlocale(LC_ALL_MASK, "zh-CN.GBK", nullptr);
  EXPECT_EQ(loc, static_cast<locale_t>(nullptr));
  EXPECT_EQ(static_cast<int>(libc_errno), EINVAL);
}

TEST(LlvmLibcLocaleCodeset, NonUtf8SetlocaleReturnsNull) {
  // POSIX does not require setlocale to set errno on failure; we match
  // glibc/musl and leave errno untouched.
  EXPECT_EQ(LIBC_NAMESPACE::setlocale(LC_ALL, "ja-JP.SJIS"),
            static_cast<char *>(nullptr));
  EXPECT_EQ(LIBC_NAMESPACE::setlocale(LC_ALL, "en-US.Big5"),
            static_cast<char *>(nullptr));
}

TEST(LlvmLibcLocaleCodeset, Utf8LocaleReportsUtf8) {
  ASSERT_NE(LIBC_NAMESPACE::setlocale(LC_ALL, "en-US.UTF-8"),
            static_cast<char *>(nullptr));
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo(CODESET), "UTF-8");
  LIBC_NAMESPACE::setlocale(LC_ALL, "C");
}

#endif // _WIN32
