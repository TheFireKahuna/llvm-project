//===-- Composite converter for strftime ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See htto_conv.times://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDIO_STRFTIME_CORE_COMPOSITE_CONVERTER_H
#define LLVM_LIBC_SRC_STDIO_STRFTIME_CORE_COMPOSITE_CONVERTER_H

#include "hdr/types/struct_tm.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/macros/config.h"
#include "src/stdio/printf_core/writer.h"
#include "src/time/strftime_core/core_structs.h"
#include "src/time/strftime_core/num_converter.h"
#include "src/time/strftime_core/str_converter.h"
#include "src/time/time_constants.h"
#include "src/time/time_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace strftime_core {

LIBC_INLINE IntFormatSection get_leading_composite_int_format(
    const tm *timeptr, const FormatSection &base_to_conv, char new_conv_name,
    int trailing_conv_len_after_component) {
  FormatSection new_conv = base_to_conv;
  new_conv.conv_name = new_conv_name;
  new_conv.min_width =
      base_to_conv.min_width > trailing_conv_len_after_component
          ? base_to_conv.min_width - trailing_conv_len_after_component
          : 0;

  IntFormatSection result = get_int_format(new_conv, timeptr);

  // Composite widths only apply to the leftmost field. If the requested width
  // is fully consumed by the suffix, emit the leading field without extra
  // padding rather than suppressing its own intrinsic formatting.
  if (base_to_conv.min_width > 0 &&
      base_to_conv.min_width <= trailing_conv_len_after_component)
    result.pad_to_len = 0;

  return result;
}

LIBC_INLINE IntFormatSection
get_trailing_composite_int_format(const tm *timeptr,
                                  const FormatSection &base_to_conv,
                                  char new_conv_name) {
  FormatSection new_conv = base_to_conv;
  new_conv.conv_name = new_conv_name;
  new_conv.min_width = 0;
  return get_int_format(new_conv, timeptr);
}

template <typename WriterT>
LIBC_INLINE int convert_date_us(WriterT *writer,
                                const FormatSection &to_conv,
                                const tm *timeptr) {
  // format is %m/%d/%y (month/day/year)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  constexpr int TRAILING_CONV_LEN = 1 + 2 + 1 + 2; // sizeof("/01/02")
  IntFormatSection year_conv;
  IntFormatSection mon_conv;
  IntFormatSection mday_conv;

  mon_conv =
      get_leading_composite_int_format(timeptr, to_conv, 'm', TRAILING_CONV_LEN);
  mday_conv = get_trailing_composite_int_format(timeptr, to_conv, 'd');
  year_conv = get_trailing_composite_int_format(timeptr, to_conv, 'y');

  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, mon_conv));
  RET_IF_RESULT_NEGATIVE(writer->write('/'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, mday_conv));
  RET_IF_RESULT_NEGATIVE(writer->write('/'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, year_conv));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_date_iso(WriterT *writer,
                                 const FormatSection &to_conv,
                                 const tm *timeptr) {
  // format is "%Y-%m-%d" (year-month-day)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  constexpr int TRAILING_CONV_LEN = 1 + 2 + 1 + 2; // sizeof("-01-02")
  IntFormatSection year_conv;
  IntFormatSection mon_conv;
  IntFormatSection mday_conv;

  year_conv =
      get_leading_composite_int_format(timeptr, to_conv, 'Y', TRAILING_CONV_LEN);
  mon_conv = get_trailing_composite_int_format(timeptr, to_conv, 'm');
  mday_conv = get_trailing_composite_int_format(timeptr, to_conv, 'd');

  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, year_conv));
  RET_IF_RESULT_NEGATIVE(writer->write('-'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, mon_conv));
  RET_IF_RESULT_NEGATIVE(writer->write('-'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, mday_conv));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_time_am_pm(WriterT *writer,
                                   const FormatSection &to_conv,
                                   const tm *timeptr) {
  // format is "%I:%M:%S %p" (hour:minute:second AM/PM)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  constexpr int TRAILING_CONV_LEN =
      1 + 2 + 1 + 2 + 1 + 2; // sizeof(":01:02 AM")
  IntFormatSection hour_conv;
  IntFormatSection min_conv;
  IntFormatSection sec_conv;

  const time_utils::TMReader time_reader(timeptr);

  hour_conv =
      get_leading_composite_int_format(timeptr, to_conv, 'I', TRAILING_CONV_LEN);
  min_conv = get_trailing_composite_int_format(timeptr, to_conv, 'M');
  sec_conv = get_trailing_composite_int_format(timeptr, to_conv, 'S');

  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, hour_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(':'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, min_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(':'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, sec_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(' '));
  RET_IF_RESULT_NEGATIVE(writer->write(time_reader.get_am_pm()));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_time_minute(WriterT *writer,
                                    const FormatSection &to_conv,
                                    const tm *timeptr) {
  // format is "%H:%M" (hour:minute)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  constexpr int TRAILING_CONV_LEN = 1 + 2; // sizeof(":01")
  IntFormatSection hour_conv;
  IntFormatSection min_conv;

  hour_conv =
      get_leading_composite_int_format(timeptr, to_conv, 'H', TRAILING_CONV_LEN);
  min_conv = get_trailing_composite_int_format(timeptr, to_conv, 'M');

  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, hour_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(':'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, min_conv));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_time_second(WriterT *writer,
                                    const FormatSection &to_conv,
                                    const tm *timeptr) {
  // format is "%H:%M:%S" (hour:minute:second)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  constexpr int TRAILING_CONV_LEN = 1 + 2 + 1 + 2; // sizeof(":01:02")
  IntFormatSection hour_conv;
  IntFormatSection min_conv;
  IntFormatSection sec_conv;

  hour_conv =
      get_leading_composite_int_format(timeptr, to_conv, 'H', TRAILING_CONV_LEN);
  min_conv = get_trailing_composite_int_format(timeptr, to_conv, 'M');
  sec_conv = get_trailing_composite_int_format(timeptr, to_conv, 'S');

  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, hour_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(':'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, min_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(':'));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, sec_conv));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_full_date_time(WriterT *writer,
                                       const FormatSection &to_conv,
                                       const tm *timeptr) {
  const time_utils::TMReader time_reader(timeptr);
  // format is "%a %b %e %T %Y" (weekday month mday [time] year)
  // we only pad the first conversion, and we assume all the other values are in
  // their valid ranges.
  // sizeof("Sun Jan 12 03:45:06 2025")
  constexpr int FULL_CONV_LEN = 3 + 1 + 3 + 1 + 2 + 1 + 8 + 1 + 4;
  // use the full conv len because this isn't being passed to a proper converter
  // that will handle the width of the leading conversion. Instead it has to be
  // handled below.
  const int requested_padding = to_conv.min_width - FULL_CONV_LEN;

  cpp::string_view wday_str = time_reader.get_locale_weekday_short_name();
  cpp::string_view month_str = time_reader.get_locale_month_short_name();
  IntFormatSection mday_conv;
  IntFormatSection year_conv;

  mday_conv = get_trailing_composite_int_format(timeptr, to_conv, 'e');
  year_conv = get_trailing_composite_int_format(timeptr, to_conv, 'Y');

  // POSIX requires a '+' prefix for years > 9999 in %c output.
  if (time_reader.get_year() > 9999)
    year_conv.sign_char = '+';

  FormatSection raw_time_conv = to_conv;
  raw_time_conv.conv_name = 'T';
  raw_time_conv.min_width = 0;

  if (requested_padding > 0)
    RET_IF_RESULT_NEGATIVE(writer->write(' ', requested_padding));
  RET_IF_RESULT_NEGATIVE(writer->write(wday_str));
  RET_IF_RESULT_NEGATIVE(writer->write(' '));
  RET_IF_RESULT_NEGATIVE(writer->write(month_str));
  RET_IF_RESULT_NEGATIVE(writer->write(' '));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, mday_conv));
  RET_IF_RESULT_NEGATIVE(writer->write(' '));
  RET_IF_RESULT_NEGATIVE(convert_time_second(writer, raw_time_conv, timeptr));
  RET_IF_RESULT_NEGATIVE(writer->write(' '));
  RET_IF_RESULT_NEGATIVE(write_padded_int(writer, year_conv));

  return WRITE_OK;
}

template <typename WriterT>
LIBC_INLINE int convert_composite(WriterT *writer,
                                  const FormatSection &to_conv,
                                  const tm *timeptr) {
  switch (to_conv.conv_name) {
  case 'c': // locale specified date and time
            // in default locale Equivalent to %a %b %e %T %Y.
    return convert_full_date_time(writer, to_conv, timeptr);
  case 'D': // %m/%d/%y (month/day/year)
    return convert_date_us(writer, to_conv, timeptr);
  case 'F': // %Y-%m-%d (year-month-day)
    return convert_date_iso(writer, to_conv, timeptr);
  case 'r': // %I:%M:%S %p (hour:minute:second AM/PM)
    return convert_time_am_pm(writer, to_conv, timeptr);
  case 'R': // %H:%M (hour:minute)
    return convert_time_minute(writer, to_conv, timeptr);
  case 'T': // %H:%M:%S (hour:minute:second)
    return convert_time_second(writer, to_conv, timeptr);
  case 'x': // locale specified date
            // in default locale Equivalent to %m/%d/%y. (same as %D)
    return convert_date_us(writer, to_conv, timeptr);
  case 'X': // locale specified time
            // in default locale Equivalent to %T.
    return convert_time_second(writer, to_conv, timeptr);
  default:
    __builtin_trap(); // this should be unreachable, but trap if you hit it.
  }
}
} // namespace strftime_core
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDIO_STRFTIME_CORE_COMPOSITE_CONVERTER_H
