//===-- Implementation of nl_langinfo -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/nl_langinfo.h"
#include "include/llvm-libc-macros/langinfo-macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"
#include "src/locale/locale.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/locale/windows/locale_data.h"
#endif

namespace LIBC_NAMESPACE_DECL {

// Buffer for nl_langinfo() return values. POSIX: "the returned pointer may
// be invalidated [...] by subsequent calls to nl_langinfo()."
static char nl_buf[128];

#ifdef LIBC_TARGET_OS_IS_WINDOWS
static bool contains_ampm_marker(const char *pattern) {
  for (size_t i = 0; pattern[i] != '\0'; ++i) {
    if (pattern[i] == '%' && pattern[i + 1] == 'p')
      return true;
  }
  return false;
}

static char *join_patterns(const char *lhs, const char *rhs) {
  size_t lhs_len = __builtin_strlen(lhs);
  size_t rhs_len = __builtin_strlen(rhs);
  if (lhs_len + rhs_len + 2 > sizeof(nl_buf))
    return nullptr;

  char joined[sizeof(nl_buf)];
  __builtin_memcpy(joined, lhs, lhs_len);
  joined[lhs_len] = ' ';
  __builtin_memcpy(joined + lhs_len + 1, rhs, rhs_len);
  joined[lhs_len + rhs_len + 1] = '\0';
  __builtin_memcpy(nl_buf, joined, lhs_len + rhs_len + 2);
  return nl_buf;
}
#endif // LIBC_TARGET_OS_IS_WINDOWS

// C locale defaults for nl_langinfo items.
static char *c_langinfo(nl_item item) {
  switch (item) {
  case CODESET:
#ifdef LIBC_TARGET_OS_IS_WINDOWS
    // POSIX: C locale uses single-byte encoding. ANSI_X3.4-1968 is the
    // standard name for the 7-bit ASCII charset (with bytes 0x80-0xFF
    // as identity-mapped codepoints in mbrtowc).
    return const_cast<char *>("ANSI_X3.4-1968");
#else
    // Upstream default: C locale uses UTF-8 (matches musl behavior).
    return const_cast<char *>("UTF-8");
#endif
  case RADIXCHAR:
    return const_cast<char *>(".");
  case THOUSEP:
    return const_cast<char *>("");
  case D_T_FMT:
    return const_cast<char *>("%a %b %e %H:%M:%S %Y");
  case D_FMT:
    return const_cast<char *>("%m/%d/%y");
  case T_FMT:
    return const_cast<char *>("%H:%M:%S");
  case T_FMT_AMPM:
    return const_cast<char *>("%I:%M:%S %p");
  case AM_STR:
    return const_cast<char *>("AM");
  case PM_STR:
    return const_cast<char *>("PM");
  case DAY_1:
    return const_cast<char *>("Sunday");
  case DAY_2:
    return const_cast<char *>("Monday");
  case DAY_3:
    return const_cast<char *>("Tuesday");
  case DAY_4:
    return const_cast<char *>("Wednesday");
  case DAY_5:
    return const_cast<char *>("Thursday");
  case DAY_6:
    return const_cast<char *>("Friday");
  case DAY_7:
    return const_cast<char *>("Saturday");
  case ABDAY_1:
    return const_cast<char *>("Sun");
  case ABDAY_2:
    return const_cast<char *>("Mon");
  case ABDAY_3:
    return const_cast<char *>("Tue");
  case ABDAY_4:
    return const_cast<char *>("Wed");
  case ABDAY_5:
    return const_cast<char *>("Thu");
  case ABDAY_6:
    return const_cast<char *>("Fri");
  case ABDAY_7:
    return const_cast<char *>("Sat");
  case MON_1:
    return const_cast<char *>("January");
  case MON_2:
    return const_cast<char *>("February");
  case MON_3:
    return const_cast<char *>("March");
  case MON_4:
    return const_cast<char *>("April");
  case MON_5:
    return const_cast<char *>("May");
  case MON_6:
    return const_cast<char *>("June");
  case MON_7:
    return const_cast<char *>("July");
  case MON_8:
    return const_cast<char *>("August");
  case MON_9:
    return const_cast<char *>("September");
  case MON_10:
    return const_cast<char *>("October");
  case MON_11:
    return const_cast<char *>("November");
  case MON_12:
    return const_cast<char *>("December");
  case ABMON_1:
    return const_cast<char *>("Jan");
  case ABMON_2:
    return const_cast<char *>("Feb");
  case ABMON_3:
    return const_cast<char *>("Mar");
  case ABMON_4:
    return const_cast<char *>("Apr");
  case ABMON_5:
    return const_cast<char *>("May");
  case ABMON_6:
    return const_cast<char *>("Jun");
  case ABMON_7:
    return const_cast<char *>("Jul");
  case ABMON_8:
    return const_cast<char *>("Aug");
  case ABMON_9:
    return const_cast<char *>("Sep");
  case ABMON_10:
    return const_cast<char *>("Oct");
  case ABMON_11:
    return const_cast<char *>("Nov");
  case ABMON_12:
    return const_cast<char *>("Dec");
  case YESEXPR:
    return const_cast<char *>("^[yY]");
  case NOEXPR:
    return const_cast<char *>("^[nN]");
  case CRNCYSTR:
    return const_cast<char *>("");
  case ERA:
  case ERA_D_FMT:
  case ERA_D_T_FMT:
  case ERA_T_FMT:
  case ALT_DIGITS:
    return const_cast<char *>("");
  default:
    return const_cast<char *>("");
  }
}

#ifdef LIBC_TARGET_OS_IS_WINDOWS

// Map nl_item to NLS blob data for the given locale record.
static char *nls_langinfo(nl_item item, const unsigned char *record) {
  int ret = 0;

  switch (item) {
  case CODESET:
    return const_cast<char *>("UTF-8");

  case RADIXCHAR:
    ret = nls::nls_record_string_utf8(record, nls::kSDecimalOffset, nl_buf,
                                      sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>(".");

  case THOUSEP:
    ret = nls::nls_record_string_utf8(record, nls::kSThousandOffset, nl_buf,
                                      sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("");

  case AM_STR:
    ret = nls::nls_record_string_utf8(record, nls::kS1159Offset, nl_buf,
                                      sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("AM");

  case PM_STR:
    ret = nls::nls_record_string_utf8(record, nls::kS2359Offset, nl_buf,
                                      sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("PM");

  case CRNCYSTR:
    ret = nls::nls_record_string_utf8(record, nls::kSCurrencyOffset, nl_buf,
                                      sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("");

  // Day names: DAY_1=Sunday(0x30), DAY_2=Monday(0x2A), ..., DAY_7=Saturday(0x2F).
  // POSIX: DAY_1=Sunday. NLS: SDAYNAME7(0x30)=Sunday, SDAYNAME1(0x2A)=Monday.
  case DAY_1:
    ret = nls::nls_day_month_name_utf8(record, 0x30, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_2:
    ret = nls::nls_day_month_name_utf8(record, 0x2A, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_3:
    ret = nls::nls_day_month_name_utf8(record, 0x2B, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_4:
    ret = nls::nls_day_month_name_utf8(record, 0x2C, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_5:
    ret = nls::nls_day_month_name_utf8(record, 0x2D, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_6:
    ret = nls::nls_day_month_name_utf8(record, 0x2E, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case DAY_7:
    ret = nls::nls_day_month_name_utf8(record, 0x2F, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);

  // Abbreviated day names.
  case ABDAY_1:
    ret = nls::nls_day_month_name_utf8(record, 0x37, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_2:
    ret = nls::nls_day_month_name_utf8(record, 0x31, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_3:
    ret = nls::nls_day_month_name_utf8(record, 0x32, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_4:
    ret = nls::nls_day_month_name_utf8(record, 0x33, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_5:
    ret = nls::nls_day_month_name_utf8(record, 0x34, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_6:
    ret = nls::nls_day_month_name_utf8(record, 0x35, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  case ABDAY_7:
    ret = nls::nls_day_month_name_utf8(record, 0x36, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);

  // Month names: MON_1=January(0x38), ..., MON_12=December(0x43).
  case MON_1: case MON_2: case MON_3: case MON_4:
  case MON_5: case MON_6: case MON_7: case MON_8:
  case MON_9: case MON_10: case MON_11: case MON_12: {
    uint32_t lctype = 0x38 + static_cast<uint32_t>(item - MON_1);
    ret = nls::nls_day_month_name_utf8(record, lctype, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  }

  // Abbreviated month names: ABMON_1(0x44), ..., ABMON_12(0x4F).
  case ABMON_1: case ABMON_2: case ABMON_3: case ABMON_4:
  case ABMON_5: case ABMON_6: case ABMON_7: case ABMON_8:
  case ABMON_9: case ABMON_10: case ABMON_11: case ABMON_12: {
    uint32_t lctype = 0x44 + static_cast<uint32_t>(item - ABMON_1);
    ret = nls::nls_day_month_name_utf8(record, lctype, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);
  }

  case D_T_FMT:
    if (nls::nls_locale_pattern_utf8(record, nls::kSLongDateOffset, nl_buf,
                                     sizeof(nl_buf)) <= 0)
      return c_langinfo(item);
    {
      char time_buf[64];
      if (nls::nls_locale_pattern_utf8(record, nls::kSTimeFormatOffset, time_buf,
                                       sizeof(time_buf)) <= 0)
        return c_langinfo(item);
      char *combined = join_patterns(nl_buf, time_buf);
      return combined ? combined : c_langinfo(item);
    }

  case D_FMT:
    ret = nls::nls_locale_pattern_utf8(record, nls::kSShortDateOffset, nl_buf,
                                       sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);

  case T_FMT:
    ret = nls::nls_locale_pattern_utf8(record, nls::kSTimeFormatOffset, nl_buf,
                                       sizeof(nl_buf));
    return ret > 0 ? nl_buf : c_langinfo(item);

  case T_FMT_AMPM:
    ret = nls::nls_locale_pattern_utf8(record, nls::kSShortTimeOffset, nl_buf,
                                       sizeof(nl_buf));
    if (ret > 0 && contains_ampm_marker(nl_buf))
      return nl_buf;
    return const_cast<char *>("");

  case YESEXPR:
    return const_cast<char *>("^[yY]");
  case NOEXPR:
    return const_cast<char *>("^[nN]");

  case ALT_DIGITS:
    ret = nls::nls_alt_digits_utf8(record, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("");

  case ERA:
    return const_cast<char *>("");

  case ERA_D_FMT:
    ret = nls::nls_era_date_pattern_utf8(record, nl_buf, sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("");

  case ERA_D_T_FMT: {
    char date_buf[64];
    char time_buf[64];
    if (nls::nls_era_date_pattern_utf8(record, date_buf, sizeof(date_buf)) <= 0)
      return const_cast<char *>("");
    if (nls::nls_locale_pattern_utf8(record, nls::kSTimeFormatOffset, time_buf,
                                     sizeof(time_buf)) <= 0)
      return const_cast<char *>("");
    char *combined = join_patterns(date_buf, time_buf);
    return combined ? combined : const_cast<char *>("");
  }

  case ERA_T_FMT: {
    char date_buf[64];
    if (nls::nls_era_date_pattern_utf8(record, date_buf, sizeof(date_buf)) <= 0)
      return const_cast<char *>("");
    ret = nls::nls_locale_pattern_utf8(record, nls::kSTimeFormatOffset, nl_buf,
                                       sizeof(nl_buf));
    return ret > 0 ? nl_buf : const_cast<char *>("");
  }

  default:
    return const_cast<char *>("");
  }
}

#endif // LIBC_TARGET_OS_IS_WINDOWS

LLVM_LIBC_FUNCTION(char *, nl_langinfo, (nl_item item)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  locale_t loc = get_current_locale();
  if (loc && loc != &c_locale) {
    // Use LC_TIME category for day/month names, LC_NUMERIC for decimal point.
    const unsigned char *record = nullptr;
    for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++) {
      if (loc->data[i] && get_nls_record(loc->data[i])) {
        record = get_nls_record(loc->data[i]);
        break;
      }
    }
    if (record)
      return nls_langinfo(item, record);
  }
#endif
  return c_langinfo(item);
}

} // namespace LIBC_NAMESPACE_DECL
