//===-- Implementation of inet_ntop function ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/arpa/inet/inet_ntop.h"

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "include/llvm-libc-macros/arpa-inet-macros.h"
#include "include/llvm-libc-macros/sys-socket-macros.h"
#include "src/__support/CPP/array.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

constexpr int IPV4_ADDR_BYTES = 4;
constexpr int IPV6_WORD_COUNT = 8;

LIBC_INLINE constexpr char hex_char(unsigned nibble) {
  return "0123456789abcdef"[nibble & 0xf];
}

// Write a decimal octet (0-255) with no leading zeros. Returns pointer past
// the last character written.
LIBC_INLINE char *write_decimal_octet(char *out, uint8_t val) {
  if (val >= 100) {
    *out++ = static_cast<char>('0' + val / 100);
    val %= 100;
    *out++ = static_cast<char>('0' + val / 10);
    *out++ = static_cast<char>('0' + val % 10);
  } else if (val >= 10) {
    *out++ = static_cast<char>('0' + val / 10);
    *out++ = static_cast<char>('0' + val % 10);
  } else {
    *out++ = static_cast<char>('0' + val);
  }
  return out;
}

// Write a 16-bit value as 1-4 lowercase hex digits (no leading zeros).
// Returns pointer past the last character written.
LIBC_INLINE char *write_hex_word(char *out, uint16_t val) {
  if (val == 0) {
    *out++ = '0';
    return out;
  }
  // Extract nibbles in reverse, then emit in forward order.
  char buf[4];
  int n = 0;
  while (val != 0) {
    buf[n++] = hex_char(val & 0xf);
    val >>= 4;
  }
  while (n > 0)
    *out++ = buf[--n];
  return out;
}

// Format an IPv4 address as "d.d.d.d". Returns the length written (excluding
// NUL terminator).
LIBC_INLINE size_t format_ipv4(const uint8_t *src, char *dst) {
  char *out = dst;
  for (int i = 0; i < IPV4_ADDR_BYTES; ++i) {
    if (i > 0)
      *out++ = '.';
    out = write_decimal_octet(out, src[i]);
  }
  *out = '\0';
  return static_cast<size_t>(out - dst);
}

// Describes a contiguous run of zero-valued words in an IPv6 address.
struct ZeroRun {
  int start;
  int length;
};

// Find the longest run of consecutive zero words (minimum length 2).
// Per RFC 5952 §4.2.3, if there are equal-length runs, the first wins.
LIBC_INLINE constexpr ZeroRun
find_longest_zero_run(const cpp::array<uint16_t, IPV6_WORD_COUNT> &words) {
  ZeroRun best{-1, 0};
  int cur_start = -1;
  int cur_len = 0;

  for (int i = 0; i < IPV6_WORD_COUNT; ++i) {
    if (words[i] == 0) {
      if (cur_start < 0)
        cur_start = i;
      ++cur_len;
    } else {
      if (cur_len > best.length) {
        best = {cur_start, cur_len};
      }
      cur_start = -1;
      cur_len = 0;
    }
  }
  if (cur_len > best.length)
    best = {cur_start, cur_len};

  // RFC 5952 §4.2.2: only elide runs of 2 or more zero words.
  if (best.length < 2)
    return {-1, 0};
  return best;
}

// Check whether the address is an IPv4-mapped IPv6 address (::ffff:x.x.x.x).
LIBC_INLINE constexpr bool
is_ipv4_mapped(const cpp::array<uint16_t, IPV6_WORD_COUNT> &words) {
  for (int i = 0; i < 5; ++i) {
    if (words[i] != 0)
      return false;
  }
  return words[5] == 0xffff;
}

// Format an IPv6 address per RFC 5952.
// Handles :: compression and ::ffff:d.d.d.d notation for IPv4-mapped
// addresses.
LIBC_INLINE size_t format_ipv6(const uint8_t *src, char *dst) {
  // Decode 8 x 16-bit words from network byte order.
  cpp::array<uint16_t, IPV6_WORD_COUNT> words{};
  for (int i = 0; i < IPV6_WORD_COUNT; ++i)
    words[i] = static_cast<uint16_t>(static_cast<uint16_t>(src[2 * i]) << 8 |
                                     src[2 * i + 1]);

  // IPv4-mapped addresses: emit "::ffff:d.d.d.d".
  if (is_ipv4_mapped(words)) {
    char *out = dst;
    out[0] = ':';
    out[1] = ':';
    out[2] = 'f';
    out[3] = 'f';
    out[4] = 'f';
    out[5] = 'f';
    out[6] = ':';
    out += 7;
    return 7 + format_ipv4(src + 12, out);
  }

  const ZeroRun gap = find_longest_zero_run(words);
  const int gap_end = (gap.start >= 0) ? gap.start + gap.length : -1;

  char *out = dst;
  for (int i = 0; i < IPV6_WORD_COUNT; ++i) {
    // Entering the :: compressed region — always emit two colons.
    // The first colon serves as the separator from the preceding word (if
    // any), and the second marks the compression. Together they form "::".
    if (i == gap.start) {
      *out++ = ':';
      *out++ = ':';
      i += gap.length - 1;
      continue;
    }

    // Separator colon between words. Suppressed before the first word and
    // immediately after :: (whose trailing colon already serves as the
    // separator).
    if (i > 0 && i != gap_end)
      *out++ = ':';

    out = write_hex_word(out, words[i]);
  }

  *out = '\0';
  return static_cast<size_t>(out - dst);
}

} // namespace

LLVM_LIBC_FUNCTION(const char *, inet_ntop,
                   (int af, const void *__restrict src, char *__restrict dst,
                    socklen_t size)) {
  char buffer[INET6_ADDRSTRLEN];
  size_t len = 0;

  switch (af) {
  case AF_INET:
    len = format_ipv4(static_cast<const uint8_t *>(src), buffer);
    break;
  case AF_INET6:
    len = format_ipv6(static_cast<const uint8_t *>(src), buffer);
    break;
  default:
    libc_errno = EAFNOSUPPORT;
    return nullptr;
  }

  if (len + 1 > size) {
    libc_errno = ENOSPC;
    return nullptr;
  }

  __builtin_memcpy(dst, buffer, len + 1);
  return dst;
}

} // namespace LIBC_NAMESPACE_DECL
