//===-- ucrt_stdio_marker.c - ISO wide-specifier link marker --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// With _CRT_STDIO_ISO_WIDE_SPECIFIERS defined, UCRT stamps every object with
// /include:__PLEASE_LINK_WITH_iso_stdio_wide_specifiers.lib and a /defaultlib
// for a library that exists only in the MSVC tools directory. The driver
// suppresses the library; this member defines the forced symbol. The
// accompanying /failifmismatch directive still rejects objects compiled with
// the legacy wide-format behaviour. The symbol name contains dots, so the
// assembly name is spelled explicitly.
//
//===----------------------------------------------------------------------===//

__attribute__((used)) const char __wincrt_iso_stdio_wide_specifiers
    __asm__("__PLEASE_LINK_WITH_iso_stdio_wide_specifiers.lib") = 0;
