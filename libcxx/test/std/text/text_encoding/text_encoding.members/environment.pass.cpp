//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: std-at-least-c++26
// REQUIRES: locale.en_US.UTF-8

// UNSUPPORTED: no-localization
// UNSUPPORTED: availability-te-environment-missing
// UNSUPPORTED: LLVM-LIBC-FIXME

// <text_encoding>

// text_encoding text_encoding::environment();

#include <cassert>
#include <clocale>
#include <format>
#include <iostream>
#include <text_encoding>

#include "platform_support.h"
#include "test_macros.h"

#if defined(_WIN32)
#  include <windows.h>

// libc++ takes the environment's encoding from the system ANSI code page, which
// is a host setting: it is 65001 (UTF-8) when the system uses UTF-8 for
// non-Unicode programs.
static std::text_encoding::id windows_acp_id() {
  switch (::GetACP()) {
  case 874:
    return std::text_encoding::windows874;
  case 932:
    return std::text_encoding::ShiftJIS;
  case 936:
    return std::text_encoding::GB2312;
  case 949:
    return std::text_encoding::KSC56011987;
  case 950:
    return std::text_encoding::Big5;
  case 1250:
    return std::text_encoding::windows1250;
  case 1251:
    return std::text_encoding::windows1251;
  case 1252:
    return std::text_encoding::windows1252;
  case 1253:
    return std::text_encoding::windows1253;
  case 1254:
    return std::text_encoding::windows1254;
  case 1255:
    return std::text_encoding::windows1255;
  case 1256:
    return std::text_encoding::windows1256;
  case 1257:
    return std::text_encoding::windows1257;
  case 1258:
    return std::text_encoding::windows1258;
  case 65001:
    return std::text_encoding::UTF8;
  default:
    return std::text_encoding::unknown;
  }
}
#endif

int main(int, char**) {
  auto check_env = []() {
#if defined(__ANDROID__)
    constexpr std::text_encoding::id expected_id = std::text_encoding::UTF8;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
    constexpr std::text_encoding::id expected_id = std::text_encoding::ASCII;
#elif defined(_WIN32)
    const std::text_encoding::id expected_id = windows_acp_id();
#elif defined(_AIX)
    constexpr std::text_encoding::id expected_id = std::text_encoding::ISOLatin1;
#else
    constexpr std::text_encoding::id expected_id = std::text_encoding::unknown;
#endif

    std::same_as<std::text_encoding> decltype(auto) te = std::text_encoding::environment();

    bool fail = false;
    if (te != expected_id) {
      std::cerr << std::format(
          "Environment mismatch: Expected ID {}, received: {{{},{}}}\n", int(expected_id), int(te.mib()), te.name());
      fail = true;
    }
#if defined(_WIN32)
    // environment_is takes the id as a template argument, so check it in both
    // directions for the two code pages Windows hosts commonly use.
    std::same_as<bool> decltype(auto) env_is_expected =
        std::text_encoding::environment_is<std::text_encoding::windows1252>() ==
            (expected_id == std::text_encoding::windows1252) &&
        std::text_encoding::environment_is<std::text_encoding::UTF8>() == (expected_id == std::text_encoding::UTF8);
#else
    std::same_as<bool> decltype(auto) env_is_expected = std::text_encoding::environment_is<expected_id>();
#endif
    if (!env_is_expected) {
      fail = true;
    }

    return !fail;
  };

  {
    // 1. Depending on the platform's default, verify that environment() returns the corresponding text encoding.
    assert(check_env());
  }

  auto te = std::text_encoding::environment();
  // 2. text_encoding::environment()'s return value isn't altered by changes to locale.
  {
    std::setlocale(LC_ALL, LOCALE_en_US_UTF_8);

    auto te2 = std::text_encoding::environment();
    assert(te == te2);
  }

  {
    std::setlocale(LC_CTYPE, LOCALE_en_US_UTF_8);

    auto te2 = std::text_encoding::environment();
    assert(te == te2);
  }
  return 0;
}
