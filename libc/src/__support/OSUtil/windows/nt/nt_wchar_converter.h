//===-- Self-hosted UTF-8 / UTF-16 character converter ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Self-hosted UTF-8 <-> UTF-16 converter for NT kernel API boundaries.
// Replaces ntdll's RtlUTF8ToUnicodeN / RtlUnicodeToUTF8N with zero external
// dependencies -- no ntdll import required for string conversion.
//
// Structural clone of upstream CharacterConverter, operating on WCHAR
// (char16_t, UTF-16LE) instead of char32_t. Surrogate pairs are handled
// internally -- callers push/pop one code unit at a time.
//
// Also provides bulk utf8_to_utf16() / utf16_to_utf8() free functions with
// an ASCII fast path for the common case (path strings are predominantly
// ASCII, so the state machine is bypassed entirely for those bytes).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WCHAR_CONVERTER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WCHAR_CONVERTER_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "hdr/types/size_t.h"
#include "src/__support/CPP/expected.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Character-at-a-time UTF-8 <-> UTF-16 converter.
///
/// Two independent directions share one instance:
///   UTF-8 -> UTF-16:  push(uint8_t) until is_full(), then pop_wchar().
///   UTF-16 -> UTF-8:  push(WCHAR)   until is_full(), then pop_utf8().
///
/// State is self-contained (no external mbstate).
class WCharacterConverter {
  char32_t partial_ = 0;
  uint8_t bytes_stored_ = 0;    // UTF-8 bytes accumulated / remaining
  uint8_t total_bytes_ = 0;     // UTF-8 byte count for current codepoint
  uint8_t utf16_remaining_ = 0; // Pending WCHAR units left to pop (0 or 1)
  bool awaiting_trail_ = false;  // push(WCHAR): high surrogate received

  static constexpr unsigned CONT_BITS = 6;
  static constexpr uint32_t CONT_MASK = 0x3F; // 10xxxxxx payload mask

public:
  LIBC_INLINE void clear() {
    partial_ = 0;
    bytes_stored_ = 0;
    total_bytes_ = 0;
    utf16_remaining_ = 0;
    awaiting_trail_ = false;
  }

  /// A complete codepoint has been accumulated and is ready to pop.
  LIBC_INLINE bool is_full() const {
    return bytes_stored_ == total_bytes_ && total_bytes_ != 0;
  }

  /// No partial data of any kind is held.
  LIBC_INLINE bool is_empty() const {
    return bytes_stored_ == 0 && !awaiting_trail_ && utf16_remaining_ == 0;
  }

  //===------------------------------------------------------------------===//
  // UTF-8 -> UTF-16: push bytes, pop WCHARs
  //===------------------------------------------------------------------===//

  /// Push one UTF-8 byte. Returns 0 on success, EILSEQ on invalid input.
  LIBC_INLINE int push(uint8_t byte) {
    if (bytes_stored_ == 0) {
      // Lead byte — determine sequence length from high-bit pattern.
      if (byte < 0x80) {
        // 0xxxxxxx: ASCII (1 byte).
        partial_ = byte;
        total_bytes_ = 1;
        bytes_stored_ = 1;
        return 0;
      }
      if (byte < 0xC2)
        // 0x80-0xBF: continuation without lead byte.
        // 0xC0-0xC1: overlong — all continuations produce codepoints < 0x80.
        return EILSEQ;
      if (byte < 0xE0) {
        // 110xxxxx: 2-byte sequence.
        partial_ = byte & 0x1F;
        total_bytes_ = 2;
        bytes_stored_ = 1;
        return 0;
      }
      if (byte < 0xF0) {
        // 1110xxxx: 3-byte sequence.
        partial_ = byte & 0x0F;
        total_bytes_ = 3;
        bytes_stored_ = 1;
        return 0;
      }
      if (byte < 0xF5) {
        // 11110xxx: 4-byte sequence.
        // 0xF5+ would lead to codepoints > U+10FFFF.
        partial_ = byte & 0x07;
        total_bytes_ = 4;
        bytes_stored_ = 1;
        return 0;
      }
      // 0xF5-0xFF: invalid lead byte.
      return EILSEQ;
    }

    // Continuation byte — must be 10xxxxxx and sequence must not be full.
    if ((byte & 0xC0) != 0x80 || is_full()) {
      clear();
      return EILSEQ;
    }
    partial_ = (partial_ << CONT_BITS) | (byte & CONT_MASK);
    ++bytes_stored_;
    return 0;
  }

  /// Pop one UTF-16 code unit. Call repeatedly after is_full() returns true.
  /// Returns Error(-1) when no output remains, Error(EILSEQ) on invalid
  /// codepoint (overlong, surrogate, out of range).
  LIBC_INLINE ErrorOr<WCHAR> pop_wchar() {
    // Stashed low surrogate from a previous supplementary-plane emission.
    if (utf16_remaining_ > 0) {
      WCHAR unit = static_cast<WCHAR>(partial_);
      partial_ = 0;
      utf16_remaining_ = 0;
      return unit;
    }

    if (!is_full())
      return Error(-1);

    char32_t cp = partial_;

    // Multi-byte validation (ASCII is always valid).
    if (total_bytes_ > 1) {
      // Overlong: the codepoint could fit in fewer bytes.
      static constexpr char32_t MIN_FOR_LEN[] = {0x80, 0x800, 0x10000};
      if (cp < MIN_FOR_LEN[total_bytes_ - 2]) {
        clear();
        return Error(EILSEQ);
      }
      // Surrogates (U+D800..U+DFFF) are not valid scalar values.
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        clear();
        return Error(EILSEQ);
      }
      // Unicode ceiling.
      if (cp > 0x10FFFF) {
        clear();
        return Error(EILSEQ);
      }
    }

    // BMP: single WCHAR.
    if (cp < 0x10000) {
      clear();
      return static_cast<WCHAR>(cp);
    }

    // Supplementary plane: emit high surrogate now, stash low for next pop.
    char32_t offset = cp - 0x10000;
    WCHAR high = static_cast<WCHAR>(0xD800 + (offset >> 10));
    WCHAR low = static_cast<WCHAR>(0xDC00 + (offset & 0x3FF));

    partial_ = static_cast<char32_t>(low);
    bytes_stored_ = 0;
    total_bytes_ = 0;
    utf16_remaining_ = 1;
    return high;
  }

  //===------------------------------------------------------------------===//
  // UTF-16 -> UTF-8: push WCHARs, pop bytes
  //===------------------------------------------------------------------===//

  /// Push one UTF-16 code unit. Returns 0 on success, EILSEQ on invalid
  /// input (lone low surrogate, non-surrogate after high surrogate).
  LIBC_INLINE int push(WCHAR unit) {
    uint16_t u = static_cast<uint16_t>(unit);

    if (awaiting_trail_) {
      // Expecting the low surrogate to complete the pair.
      if (u < 0xDC00 || u > 0xDFFF) {
        clear();
        return EILSEQ;
      }
      // Combine surrogate pair into a scalar value.
      partial_ = 0x10000 + ((partial_ - 0xD800) << 10) + (u - 0xDC00);
      awaiting_trail_ = false;
      total_bytes_ = 4; // Supplementary plane: always 4 UTF-8 bytes.
      bytes_stored_ = 4;
      return 0;
    }

    if (!is_empty())
      return -1; // Previous codepoint not fully popped yet.

    // High surrogate: stash and wait for the low half.
    if (u >= 0xD800 && u <= 0xDBFF) {
      partial_ = static_cast<char32_t>(u);
      awaiting_trail_ = true;
      return 0;
    }
    // Lone low surrogate: invalid.
    if (u >= 0xDC00 && u <= 0xDFFF)
      return EILSEQ;

    // BMP scalar value.
    partial_ = static_cast<char32_t>(u);
    if (u <= 0x7F)
      total_bytes_ = 1;
    else if (u <= 0x7FF)
      total_bytes_ = 2;
    else
      total_bytes_ = 3; // BMP ceiling is 0xFFFF.
    bytes_stored_ = total_bytes_;
    return 0;
  }

  /// Pop one UTF-8 byte. Call repeatedly after is_full() returns true.
  /// Emits the lead byte first, then continuation bytes in order.
  /// Returns Error(-1) when no output remains.
  LIBC_INLINE ErrorOr<uint8_t> pop_utf8() {
    if (bytes_stored_ == 0)
      return Error(-1);

    static constexpr uint8_t LEAD_HEADER[] = {0x00, 0xC0, 0xE0, 0xF0};

    unsigned shift = (bytes_stored_ - 1) * CONT_BITS;
    uint8_t out;
    if (bytes_stored_ == total_bytes_) {
      // Lead byte: length-encoded header + most-significant payload bits.
      out = LEAD_HEADER[total_bytes_ - 1] |
            static_cast<uint8_t>(partial_ >> shift);
    } else {
      // Continuation byte: 10xxxxxx.
      out = 0x80 | static_cast<uint8_t>((partial_ >> shift) & CONT_MASK);
    }

    --bytes_stored_;
    if (bytes_stored_ == 0)
      clear();
    return out;
  }
};

//===----------------------------------------------------------------------===//
// Bulk conversion functions
//===----------------------------------------------------------------------===//

/// Convert UTF-8 to UTF-16. Returns the number of WCHARs written (excluding
/// any NUL terminator -- callers NUL-terminate if needed).
///
/// If dst is nullptr, performs a dry run and returns the required WCHAR count.
/// On error, returns a negative errno: -EILSEQ for invalid UTF-8,
/// -ENAMETOOLONG if the destination buffer is too small.
LIBC_INLINE int utf8_to_utf16(const char *src, size_t src_bytes, WCHAR *dst,
                              size_t dst_wchars) {
  size_t si = 0, di = 0;

  while (si < src_bytes) {
    uint8_t b = static_cast<uint8_t>(src[si]);

    // ASCII fast path: single byte -> single WCHAR, no state machine.
    if (b < 0x80) {
      if (dst) {
        if (di >= dst_wchars)
          return -ENAMETOOLONG;
        dst[di] = static_cast<WCHAR>(b);
      }
      ++si;
      ++di;
      continue;
    }

    // Multi-byte sequence: push bytes until the codepoint is complete.
    WCharacterConverter conv;
    do {
      if (conv.push(b))
        return -EILSEQ;
      ++si;
      if (conv.is_full())
        break;
      if (si >= src_bytes)
        return -EILSEQ; // Truncated sequence.
      b = static_cast<uint8_t>(src[si]);
    } while (true);

    // Pop UTF-16 code units (1 for BMP, 2 for supplementary).
    for (;;) {
      auto r = conv.pop_wchar();
      if (!r.has_value()) {
        if (r.error() > 0)
          return -r.error(); // Validation failure (EILSEQ).
        break;               // No more output for this codepoint.
      }
      if (dst) {
        if (di >= dst_wchars)
          return -ENAMETOOLONG;
        dst[di] = r.value();
      }
      ++di;
    }
  }

  return static_cast<int>(di);
}

/// Convert UTF-16 to UTF-8. Returns the number of bytes written (excluding
/// any NUL terminator).
///
/// If dst is nullptr, performs a dry run and returns the required byte count.
/// On error, returns a negative errno: -EILSEQ for invalid UTF-16 (lone
/// surrogates), -ENAMETOOLONG if the destination buffer is too small.
LIBC_INLINE int utf16_to_utf8(const WCHAR *src, size_t src_wchars, char *dst,
                              size_t dst_bytes) {
  size_t si = 0, di = 0;

  while (si < src_wchars) {
    uint16_t u = static_cast<uint16_t>(src[si]);

    // ASCII fast path: single WCHAR -> single byte.
    if (u < 0x80) {
      if (dst) {
        if (di >= dst_bytes)
          return -ENAMETOOLONG;
        dst[di] = static_cast<char>(u);
      }
      ++si;
      ++di;
      continue;
    }

    // Push WCHAR units until a complete codepoint.
    WCharacterConverter conv;
    do {
      if (conv.push(src[si]))
        return -EILSEQ;
      ++si;
      if (conv.is_full())
        break;
      if (si >= src_wchars)
        return -EILSEQ; // Lone high surrogate at end.
    } while (true);

    // Pop UTF-8 bytes.
    for (;;) {
      auto r = conv.pop_utf8();
      if (!r.has_value()) {
        if (r.error() > 0)
          return -r.error();
        break;
      }
      if (dst) {
        if (di >= dst_bytes)
          return -ENAMETOOLONG;
        dst[di] = static_cast<char>(r.value());
      }
      ++di;
    }
  }

  return static_cast<int>(di);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_NT_WCHAR_CONVERTER_H
