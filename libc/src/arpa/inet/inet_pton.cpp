//===-- Implementation of inet_pton function ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/arpa/inet/inet_pton.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/CPP/array.h"
#include "src/__support/common.h"
#include "src/__support/ctype_utils.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

constexpr int IPV4_ADDR_BYTES = 4;
constexpr int IPV6_ADDR_BYTES = 16;
constexpr int MAX_HEX_DIGITS_PER_WORD = 4;
constexpr int MAX_DEC_DIGITS_PER_OCTET = 3;

// Returns 0-15 for valid hex digits, -1 otherwise.
// Switch form is encoding-independent (no assumption of contiguous ranges).
LIBC_INLINE constexpr int hex_val(char ch) {
  switch (ch) {
  case '0':
    return 0;
  case '1':
    return 1;
  case '2':
    return 2;
  case '3':
    return 3;
  case '4':
    return 4;
  case '5':
    return 5;
  case '6':
    return 6;
  case '7':
    return 7;
  case '8':
    return 8;
  case '9':
    return 9;
  case 'a':
  case 'A':
    return 10;
  case 'b':
  case 'B':
    return 11;
  case 'c':
  case 'C':
    return 12;
  case 'd':
  case 'D':
    return 13;
  case 'e':
  case 'E':
    return 14;
  case 'f':
  case 'F':
    return 15;
  default:
    return -1;
  }
}

// Parse a strict dotted-decimal IPv4 address (no leading zeros, no octal).
// On success, writes 4 bytes to dst and returns a pointer past the last
// consumed character. Returns nullptr on invalid input.
LIBC_INLINE const char *parse_ipv4(const char *src, uint8_t *dst) {
  for (int octet = 0; octet < IPV4_ADDR_BYTES; ++octet) {
    if (!internal::isdigit(*src))
      return nullptr;

    if (*src == '0') {
      // A bare '0' is valid; '01', '00', etc. are rejected (no octal).
      dst[octet] = 0;
      ++src;
      if (internal::isdigit(*src))
        return nullptr;
    } else {
      unsigned val = 0;
      for (int d = 0; d < MAX_DEC_DIGITS_PER_OCTET && internal::isdigit(*src);
           ++d) {
        val = val * 10 + static_cast<unsigned>(*src - '0');
        ++src;
      }
      if (val > 255)
        return nullptr;
      dst[octet] = static_cast<uint8_t>(val);
    }

    if (octet < IPV4_ADDR_BYTES - 1) {
      if (*src != '.')
        return nullptr;
      ++src;
    }
  }
  return src;
}

// Parse an IPv6 text address into 16 bytes.
// Supports :: compression and embedded IPv4 suffixes (e.g., ::ffff:1.2.3.4).
LIBC_INLINE bool parse_ipv6(const char *src, uint8_t *dst) {
  cpp::array<uint8_t, IPV6_ADDR_BYTES> buf{};
  uint8_t *cur = buf.data();
  uint8_t *const end = buf.data() + IPV6_ADDR_BYTES;
  uint8_t *gap = nullptr; // Marks the :: expansion point.

  // Handle leading "::".
  if (src[0] == ':') {
    if (src[1] != ':')
      return false;
    gap = cur;
    src += 2;
  }

  while (*src != '\0') {
    if (cur >= end)
      return false;

    // Consume up to 4 hex digits.
    const char *word_start = src;
    unsigned val = 0;
    int digits = 0;
    while (digits < MAX_HEX_DIGITS_PER_WORD) {
      int h = hex_val(*src);
      if (h < 0)
        break;
      val = (val << 4) | static_cast<unsigned>(h);
      ++src;
      ++digits;
    }

    // A '.' after digits means an embedded IPv4 tail (e.g., ::ffff:1.2.3.4).
    if (*src == '.') {
      if (cur + IPV4_ADDR_BYTES > end)
        return false;
      const char *after = parse_ipv4(word_start, cur);
      if (after == nullptr)
        return false;
      cur += IPV4_ADDR_BYTES;
      src = after;
      break;
    }

    // Must have consumed at least one hex digit to form a valid word.
    if (digits == 0)
      return false;

    // Store the 16-bit word in network byte order.
    if (cur + 2 > end)
      return false;
    *cur++ = static_cast<uint8_t>(val >> 8);
    *cur++ = static_cast<uint8_t>(val & 0xff);

    if (*src == '\0')
      break;
    if (*src != ':')
      return false;
    ++src;

    if (*src == ':') {
      // Double colon — only one :: per address.
      if (gap != nullptr)
        return false;
      gap = cur;
      ++src;
      if (*src == '\0')
        break;
    } else if (*src == '\0') {
      // Trailing single colon (e.g., "1:2:") is only valid after ::.
      if (gap == nullptr)
        return false;
      break;
    }
  }

  // Expand :: by shifting the tail to the end of the buffer.
  if (gap != nullptr) {
    if (cur == end)
      return false; // No room for expansion — :: is redundant.

    const size_t tail_len = static_cast<size_t>(cur - gap);
    // Shift tail bytes to the end of the buffer.
    for (size_t i = 0; i < tail_len; ++i)
      buf[IPV6_ADDR_BYTES - 1 - i] = gap[tail_len - 1 - i];
    // Zero-fill the gap.
    for (uint8_t *p = gap;
         p < buf.data() + IPV6_ADDR_BYTES - tail_len; ++p)
      *p = 0;
    cur = end;
  }

  if (cur != end)
    return false;

  __builtin_memcpy(dst, buf.data(), IPV6_ADDR_BYTES);
  return true;
}

} // namespace

LLVM_LIBC_FUNCTION(int, inet_pton,
                   (int af, const char *__restrict src, void *__restrict dst)) {
  switch (af) {
  case AF_INET: {
    auto *out = static_cast<uint8_t *>(dst);
    const char *tail = parse_ipv4(src, out);
    // The entire string must be consumed for a valid AF_INET address.
    return (tail != nullptr && *tail == '\0') ? 1 : 0;
  }
  case AF_INET6:
    return parse_ipv6(src, static_cast<uint8_t *>(dst)) ? 1 : 0;
  default:
    libc_errno = EAFNOSUPPORT;
    return -1;
  }
}

} // namespace LIBC_NAMESPACE_DECL
