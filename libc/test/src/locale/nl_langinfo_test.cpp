//===-- Unittests for nl_langinfo -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/locale_macros.h"
#include "include/llvm-libc-macros/langinfo-macros.h"
#include "src/locale/freelocale.h"
#include "src/locale/newlocale.h"
#include "src/locale/nl_langinfo_l.h"
#include "test/UnitTest/Test.h"

TEST(LlvmLibcLanginfo, CLocaleDefaults) {
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(D_FMT, LC_GLOBAL_LOCALE),
               "%m/%d/%y");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ALT_DIGITS, LC_GLOBAL_LOCALE), "");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_D_FMT, LC_GLOBAL_LOCALE), "");
}

#if defined(_WIN32) || defined(_WIN32_ITANIUM)

TEST(LlvmLibcLanginfo, EnUsPatterns) {
  locale_t locale = LIBC_NAMESPACE::newlocale(LC_ALL, "en-US", nullptr);
  ASSERT_NE(locale, static_cast<locale_t>(nullptr));

  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(D_FMT, locale), "%m/%d/%Y");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(T_FMT, locale), "%I:%M:%S %p");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(T_FMT_AMPM, locale), "%I:%M %p");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_D_FMT, locale), "");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ALT_DIGITS, locale), "");

  LIBC_NAMESPACE::freelocale(locale);
}

TEST(LlvmLibcLanginfo, JaJPEraPatterns) {
  locale_t locale = LIBC_NAMESPACE::newlocale(LC_ALL, "ja-JP", nullptr);
  ASSERT_NE(locale, static_cast<locale_t>(nullptr));

  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(D_FMT, locale), "%Y/%m/%d");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(T_FMT, locale), "%H:%M:%S");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_D_FMT, locale),
               "%EC%Ey年%m月%d日");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_D_T_FMT, locale),
               "%EC%Ey年%m月%d日 %H:%M:%S");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_T_FMT, locale), "%H:%M:%S");

  LIBC_NAMESPACE::freelocale(locale);
}

TEST(LlvmLibcLanginfo, ThaiAltDigitsAndEraDate) {
  locale_t locale = LIBC_NAMESPACE::newlocale(LC_ALL, "th-TH", nullptr);
  ASSERT_NE(locale, static_cast<locale_t>(nullptr));

  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ALT_DIGITS, locale),
               "๐;๑;๒;๓;๔;๕;๖;๗;๘;๙");
  EXPECT_STREQ(LIBC_NAMESPACE::nl_langinfo_l(ERA_D_FMT, locale), "%d/%m/%Ey");

  LIBC_NAMESPACE::freelocale(locale);
}

#endif // _WIN32
