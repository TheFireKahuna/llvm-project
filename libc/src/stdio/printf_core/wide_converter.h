//===-- Wide conversion dispatch for wprintf ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Dispatches wide printf conversions. %s, %c, and %n get wide-specific
// implementations. All other conversions reuse the existing narrow converters
// via a narrow-to-wide bridge that widens output through mbrtowc.
//
// All functions are templated on WideWriterT so that both buffer-based
// (swprintf) and stream-based (fwprintf) writers work without virtual dispatch.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_CONVERTER_H
#define LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_CONVERTER_H

#include "hdr/types/wchar_t.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/mbrtowc.h"
#include "src/__support/wchar/mbstate.h"
#include "src/stdio/printf_core/converter.h"
#include "src/stdio/printf_core/core_structs.h"
#include "src/stdio/printf_core/writer.h"

#include <inttypes.h>
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace printf_core {

// Context for the narrow-to-wide bridge. Persists mbstate across flushes so
// that multi-byte sequences split at chunk boundaries are handled correctly.
// The void* pointer + static hook function allow type-erased forwarding
// through FlushingBuffer's callback interface.
template <typename WideWriterT> struct WidenContext {
  WideWriterT *writer;
  internal::mbstate state{};

  static LIBC_INLINE int hook(cpp::string_view narrow, void *ctx) {
    auto *wctx = static_cast<WidenContext *>(ctx);
    const char *src = narrow.data();
    const char *end = src + narrow.size();

    while (src < end) {
      wchar_t wc;
      auto ret = internal::mbrtowc(&wc, src, static_cast<size_t>(end - src),
                                   &wctx->state);
      if (!ret.has_value())
        return MB_CONVERSION_ERROR;
      size_t n = ret.value();
      if (n == static_cast<size_t>(-2)) {
        // Incomplete sequence at chunk boundary — mbstate records progress.
        return WRITE_OK;
      }
      if (n == 0) {
        n = 1;
        wc = L'\0';
      }
      int r = wctx->writer->write(wc);
      if (r != WRITE_OK)
        return r;
      src += n;
    }
    return WRITE_OK;
  }
};

// Run a narrow converter and widen its output into the wide writer.
template <typename WideWriterT>
LIBC_INLINE int narrow_to_wide(WideWriterT *wwriter,
                               const FormatSection &to_conv) {
  WidenContext<WideWriterT> wctx{wwriter, {}};
  constexpr size_t BRIDGE_BUFF_SIZE = 1024;
  char bridge_buff[BRIDGE_BUFF_SIZE];
  FlushingBuffer fb(bridge_buff, BRIDGE_BUFF_SIZE,
                    &WidenContext<WideWriterT>::hook, &wctx);
  Writer writer(fb);

  int result = convert(&writer, to_conv);
  if (result != WRITE_OK)
    return result;
  return fb.flush_to_stream();
}

// Wide %c and %lc (C11 §7.29.2.1).
template <typename WideWriterT>
LIBC_INLINE int wide_convert_char(WideWriterT *writer,
                                  const FormatSection &to_conv) {
  wchar_t wc;

  if (to_conv.length_modifier == LengthModifier::l) {
    // %lc: wint_t argument, written directly.
    auto wi = static_cast<wchar_t>(to_conv.conv_val_raw);
    wc = wi;
  } else {
    // %c: int argument, converted as if by btowc (C11 §7.29.2.1p8).
    // In UTF-8 (the only encoding llvm-libc supports), btowc is only
    // defined for single-byte characters, i.e. the ASCII range 0-127.
    int c = static_cast<int>(to_conv.conv_val_raw);
    if (c < 0 || c > 0x7f)
      return ILLEGAL_WIDE_CHAR;
    wc = static_cast<wchar_t>(c);
  }

  int padding = to_conv.min_width > 1 ? to_conv.min_width - 1 : 0;

  if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) == 0)
    RET_IF_RESULT_NEGATIVE(writer->write(static_cast<wchar_t>(L' '), padding));

  RET_IF_RESULT_NEGATIVE(writer->write(wc));

  if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) != 0)
    RET_IF_RESULT_NEGATIVE(writer->write(static_cast<wchar_t>(L' '), padding));

  return WRITE_OK;
}

// Wide %s and %ls (C11 §7.29.2.1).
template <typename WideWriterT>
LIBC_INLINE int wide_convert_string(WideWriterT *writer,
                                    const FormatSection &to_conv) {
  if (to_conv.length_modifier == LengthModifier::l) {
    // %ls: wchar_t* argument, written directly.
    const wchar_t *ws =
        reinterpret_cast<const wchar_t *>(to_conv.conv_val_ptr);

#ifndef LIBC_COPT_PRINTF_NO_NULLPTR_CHECKS
    if (ws == nullptr) {
      static constexpr wchar_t null_str[] = {'(', 'n', 'u', 'l', 'l', ')', '\0'};
      ws = null_str;
    }
#endif

    size_t len = 0;
    while (ws[len] != wchar_t('\0')) {
      if (to_conv.precision >= 0 &&
          len >= static_cast<size_t>(to_conv.precision))
        break;
      ++len;
    }

    int padding = to_conv.min_width > static_cast<int>(len)
                      ? to_conv.min_width - static_cast<int>(len)
                      : 0;

    if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) == 0)
      RET_IF_RESULT_NEGATIVE(
          writer->write(static_cast<wchar_t>(L' '), padding));

    RET_IF_RESULT_NEGATIVE(writer->write_wide(ws, len));

    if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) != 0)
      RET_IF_RESULT_NEGATIVE(
          writer->write(static_cast<wchar_t>(L' '), padding));

    return WRITE_OK;
  }

  // %s: char* argument, converted to wide via mbrtowc.
  const char *s = reinterpret_cast<const char *>(to_conv.conv_val_ptr);

#ifndef LIBC_COPT_PRINTF_NO_NULLPTR_CHECKS
  if (s == nullptr)
    s = "(null)";
#endif

  // Compute string length for remaining-bytes calculation.
  size_t s_len = 0;
  while (s[s_len] != '\0')
    ++s_len;

  // First pass: count wide characters (precision limits wide chars written).
  size_t wide_len = 0;
  {
    internal::mbstate count_state{};
    const char *p = s;
    while (*p != '\0') {
      wchar_t wc;
      size_t remaining = s_len - static_cast<size_t>(p - s);
      auto ret = internal::mbrtowc(&wc, p, remaining, &count_state);
      if (!ret.has_value())
        return MB_CONVERSION_ERROR;
      size_t n = ret.value();
      if (n == 0 || n == static_cast<size_t>(-2))
        break;
      if (to_conv.precision >= 0 &&
          wide_len >= static_cast<size_t>(to_conv.precision))
        break;
      ++wide_len;
      p += n;
    }
  }

  int padding = to_conv.min_width > static_cast<int>(wide_len)
                    ? to_conv.min_width - static_cast<int>(wide_len)
                    : 0;

  if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) == 0)
    RET_IF_RESULT_NEGATIVE(
        writer->write(static_cast<wchar_t>(L' '), padding));

  // Second pass: convert and write.
  {
    internal::mbstate conv_state{};
    const char *p = s;
    for (size_t i = 0; i < wide_len; ++i) {
      wchar_t wc;
      size_t remaining = s_len - static_cast<size_t>(p - s);
      auto ret = internal::mbrtowc(&wc, p, remaining, &conv_state);
      size_t n = ret.value();
      if (n == 0)
        break;
      RET_IF_RESULT_NEGATIVE(writer->write(wc));
      p += n;
    }
  }

  if (padding > 0 && (to_conv.flags & FormatFlags::LEFT_JUSTIFIED) != 0)
    RET_IF_RESULT_NEGATIVE(
        writer->write(static_cast<wchar_t>(L' '), padding));

  return WRITE_OK;
}

// Wide %n: stores the number of wide characters written so far.
template <typename WideWriterT>
LIBC_INLINE int wide_convert_write_int(WideWriterT *writer,
                                       const FormatSection &to_conv) {
#ifndef LIBC_COPT_PRINTF_NO_NULLPTR_CHECKS
  if (to_conv.conv_val_ptr == nullptr)
    return NULLPTR_WRITE_ERROR;
#endif

  size_t written = writer->get_chars_written();

  switch (to_conv.length_modifier) {
  case LengthModifier::none:
    *reinterpret_cast<int *>(to_conv.conv_val_ptr) =
        static_cast<int>(written);
    break;
  case LengthModifier::l:
    *reinterpret_cast<long *>(to_conv.conv_val_ptr) =
        static_cast<long>(written);
    break;
  case LengthModifier::ll:
  case LengthModifier::L:
    *reinterpret_cast<long long *>(to_conv.conv_val_ptr) = written;
    break;
  case LengthModifier::h:
    *reinterpret_cast<short *>(to_conv.conv_val_ptr) =
        static_cast<short>(written);
    break;
  case LengthModifier::hh:
    *reinterpret_cast<signed char *>(to_conv.conv_val_ptr) =
        static_cast<signed char>(written);
    break;
  case LengthModifier::z:
    *reinterpret_cast<size_t *>(to_conv.conv_val_ptr) = written;
    break;
  case LengthModifier::t:
    *reinterpret_cast<ptrdiff_t *>(to_conv.conv_val_ptr) = written;
    break;
  case LengthModifier::j:
  case LengthModifier::w:
  case LengthModifier::wf:
    *reinterpret_cast<uintmax_t *>(to_conv.conv_val_ptr) = written;
    break;
  }
  return WRITE_OK;
}

// Main dispatch for wide printf conversions.
template <typename WideWriterT>
LIBC_INLINE int wide_convert(WideWriterT *writer,
                             const FormatSection &to_conv) {
  if (!to_conv.has_conv)
    return WRITE_OK;

  switch (to_conv.conv_name) {
  case '%':
    return writer->write(static_cast<wchar_t>(L'%'));
  case 'c':
    return wide_convert_char(writer, to_conv);
  case 's':
    return wide_convert_string(writer, to_conv);
#ifndef LIBC_COPT_PRINTF_DISABLE_WRITE_INT
  case 'n':
    return wide_convert_write_int(writer, to_conv);
#endif
  default:
    return narrow_to_wide(writer, to_conv);
  }
}

} // namespace printf_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_PRINTF_CORE_WIDE_CONVERTER_H
