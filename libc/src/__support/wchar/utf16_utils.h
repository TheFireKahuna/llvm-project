//===-- UTF-16 helpers for wchar conversions --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_WCHAR_UTF16_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_WCHAR_UTF16_UTILS_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/char32_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/wchar_t.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct DecodedWChar {
  char32_t value;
  size_t units;
};

// Surrogate detection and encoding are only meaningful when wchar_t is 16-bit
// (UTF-16). With 32-bit wchar_t (POSIX), every wchar_t is a complete scalar.

LIBC_INLINE constexpr bool is_utf16_high_surrogate(wchar_t wc) {
  if constexpr (sizeof(wchar_t) == 2) {
    uint16_t value = static_cast<uint16_t>(wc);
    return value >= 0xD800 && value <= 0xDBFF;
  } else {
    (void)wc;
    return false;
  }
}

LIBC_INLINE constexpr bool is_utf16_low_surrogate(wchar_t wc) {
  if constexpr (sizeof(wchar_t) == 2) {
    uint16_t value = static_cast<uint16_t>(wc);
    return value >= 0xDC00 && value <= 0xDFFF;
  } else {
    (void)wc;
    return false;
  }
}

LIBC_INLINE constexpr bool is_utf16_surrogate(wchar_t wc) {
  if constexpr (sizeof(wchar_t) == 2)
    return is_utf16_high_surrogate(wc) || is_utf16_low_surrogate(wc);
  else {
    (void)wc;
    return false;
  }
}

LIBC_INLINE ErrorOr<size_t> encode_wchar_scalar(char32_t scalar,
                                                wchar_t *dst) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU))
      return Error(EILSEQ);

    if (scalar < 0x10000U) {
      if (dst != nullptr)
        dst[0] = static_cast<wchar_t>(scalar);
      return 1;
    }

    char32_t plane = scalar - 0x10000U;
    if (dst != nullptr) {
      dst[0] = static_cast<wchar_t>(0xD800U + (plane >> 10));
      dst[1] = static_cast<wchar_t>(0xDC00U + (plane & 0x3FFU));
    }
    return 2;
  } else {
    if (dst != nullptr)
      dst[0] = static_cast<wchar_t>(scalar);
    return 1;
  }
}

LIBC_INLINE ErrorOr<DecodedWChar> decode_wchar_scalar(const wchar_t *src,
                                                      size_t src_len) {
  if (src_len == 0)
    return Error(-1);

  if constexpr (sizeof(wchar_t) == 2) {
    uint16_t lead = static_cast<uint16_t>(src[0]);
    if (lead < 0xD800U || lead > 0xDFFFU)
      return DecodedWChar{static_cast<char32_t>(lead), 1};

    if (lead >= 0xDC00U)
      return Error(EILSEQ);

    if (src_len < 2)
      return Error(-1);

    uint16_t trail = static_cast<uint16_t>(src[1]);
    if (trail < 0xDC00U || trail > 0xDFFFU)
      return Error(EILSEQ);

    return DecodedWChar{
        static_cast<char32_t>(0x10000U +
                              ((static_cast<uint32_t>(lead) - 0xD800U) << 10) +
                              (static_cast<uint32_t>(trail) - 0xDC00U)),
        2};
  } else {
    return DecodedWChar{static_cast<char32_t>(src[0]), 1};
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_WCHAR_UTF16_UTILS_H
