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

int main(int, char**) {
  // On Windows, changes to the "LANG" environment variable don't affect the result
  // of std::text_encoding::environment() and environment_is()
  auto te = std::text_encoding::environment();

  ::SetEnvironmentVariableA("LANG", LOCALE_fr_CA_ISO8859_1);

  // The ANSI code page, which libc++ follows, is a host setting; 65001 (UTF-8)
  // and 1252 are the two hosts commonly have.
  UINT acp = ::GetACP();
  if (acp != 1252 && acp != 65001)
    return 0;
  const auto expected_id = acp == 65001 ? std::text_encoding::id::UTF8 : std::text_encoding::id::windows1252;
  assert(std::text_encoding::environment_is<std::text_encoding::id::windows1252>() ==
         (expected_id == std::text_encoding::id::windows1252));
  assert(std::text_encoding::environment_is<std::text_encoding::id::UTF8>() ==
         (expected_id == std::text_encoding::id::UTF8));
  assert(te == std::text_encoding::environment());
  assert(te.mib() == expected_id);

  return 0;
}
