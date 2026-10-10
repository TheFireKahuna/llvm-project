//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <text_encoding>

// REQUIRES: std-at-least-c++26
// REQUIRES: locale.fr_CA.ISO8859-1

// UNSUPPORTED: no-localization
// UNSUPPORTED: availability-te-environment-missing
// REQUIRES: windows

// std::text_encoding::environment()

#include <cassert>
#include <cstdlib>
#include <text_encoding>
#include <windows.h>

#include "platform_support.h" // locale name macros

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

int main(int, char**) {
  // On Windows, changes to the "LANG" environment variable don't affect the result
  // of std::text_encoding::environment() and environment_is()
  auto te = std::text_encoding::environment();

  ::SetEnvironmentVariableA("LANG", LOCALE_fr_CA_ISO8859_1);

  const std::text_encoding::id expected_id = windows_acp_id();
  assert(std::text_encoding::environment_is<std::text_encoding::id::windows1252>() ==
         (expected_id == std::text_encoding::id::windows1252));
  assert(std::text_encoding::environment_is<std::text_encoding::id::UTF8>() ==
         (expected_id == std::text_encoding::id::UTF8));
  assert(te == std::text_encoding::environment());
  assert(te.mib() == expected_id);

  return 0;
}
