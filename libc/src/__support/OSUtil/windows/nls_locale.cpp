//===-- NLS locale blob parser implementation -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"

#include "include/llvm-libc-types/struct_lconv.h"

#include <limits.h>

namespace LIBC_NAMESPACE_DECL {
namespace nls {

//===----------------------------------------------------------------------===//
// Typed accessors for PCB NLS state.
//===----------------------------------------------------------------------===//

static uint8_t *blob() { return static_cast<uint8_t *>(g_pcb.nls.blob); }
static LocaleHeader *header() { return g_pcb.nls.header; }
static uint8_t *records() { return g_pcb.nls.records; }
static uint8_t *calendar_records() {
  return g_pcb.nls.calendar_records;
}
static WCHAR *strings() { return g_pcb.nls.strings; }
static LcidEntry *lcid_table() { return g_pcb.nls.lcid_table; }
static NameEntry *name_table() { return g_pcb.nls.name_table; }

static uint16_t read_u16(const void *src) {
  uint16_t value = 0;
  __builtin_memcpy(&value, src, sizeof(value));
  return value;
}

static uint32_t read_u32(const void *src) {
  uint32_t value = 0;
  __builtin_memcpy(&value, src, sizeof(value));
  return value;
}

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

// InitFn for the LazyInit gate. Returns 0 unconditionally: an
// NtInitializeNlsFiles failure leaves `g_pcb.nls.blob` null and is
// reported to callers via `nls_init()` returning false. Aborting the
// process would be wrong — locale lookup failing is a recoverable
// error (callers fall back to "C" behavior).
//
// Not registered in `.libclzr`: the NT NLS blob is immutable
// process-lifetime kernel data. It survives exec (the new image sees
// the same blob via the same NtInitializeNlsFiles call), so re-running
// init post-exec is pure waste.
static int nls_init_impl() {
  LARGE_INTEGER casing_size{};
  ULONG nls_version = 0;
  ULONG default_lcid = 0;

  // Use local for the NtInitializeNlsFiles out-parameter, then store to PCB.
  uint8_t *blob_ptr = nullptr;
  NTSTATUS status = ::NtInitializeNlsFiles(
      reinterpret_cast<PVOID *>(&blob_ptr), &default_lcid, &casing_size,
      &nls_version);
  if (!NT_SUCCESS(status))
    return 0;

  g_pcb.nls.blob = blob_ptr;
  g_pcb.nls.default_lcid = default_lcid;

  auto *blob_hdr = reinterpret_cast<BlobHeader *>(blob_ptr);
  uint8_t *sec = blob_ptr + blob_hdr->locale_section;

  // The section starts with a DWORD sub-offset to the locale header.
  uint32_t sub_off = read_u32(sec);
  auto *hdr = reinterpret_cast<LocaleHeader *>(sec + sub_off);
  g_pcb.nls.header = hdr;

  g_pcb.nls.records = sec + hdr->records_offset;
  g_pcb.nls.calendar_records = sec + hdr->calendar_records_offset;
  g_pcb.nls.strings = reinterpret_cast<WCHAR *>(sec + hdr->string_pool_offset);
  g_pcb.nls.lcid_table =
      reinterpret_cast<LcidEntry *>(sec + hdr->lcid_table_offset);
  g_pcb.nls.name_table =
      reinterpret_cast<NameEntry *>(sec + hdr->name_table_offset);
  return 0;
}

static ::LIBC_NAMESPACE::internal::LazyInit<&nls_init_impl> g_nls_init;

bool nls_init() {
  g_nls_init.ensure();
  return blob() != nullptr;
}

//===----------------------------------------------------------------------===//
// String pool access
//===----------------------------------------------------------------------===//

const WCHAR *nls_string(uint32_t pool_index, int *out_len) {
  uint16_t len = strings()[pool_index];
  if (out_len)
    *out_len = len;
  return &strings()[pool_index + 1];
}

const WCHAR *nls_string_pool() { return strings(); }

//===----------------------------------------------------------------------===//
// UTF-16 → UTF-8 conversion (self-hosted)
//===----------------------------------------------------------------------===//

static int wide_to_utf8(const WCHAR *src, int src_chars, char *dst,
                        size_t dst_len) {
  if (src_chars == 0) {
    if (dst && dst_len > 0)
      dst[0] = '\0';
    return 1;
  }

  size_t avail = dst_len > 0 ? dst_len - 1 : 0;
  int n = windows::utf16_to_utf8(src, static_cast<size_t>(src_chars), dst, avail);
  if (n < 0)
    return 0;

  if (dst && dst_len > 0)
    dst[n] = '\0';
  return n + 1;
}

//===----------------------------------------------------------------------===//
// Record string access
//===----------------------------------------------------------------------===//

int nls_record_string_utf8(const uint8_t *record, uint32_t field_offset,
                           char *dst, size_t dst_len) {
  uint32_t pool_idx = read_u32(record + field_offset);

  int wlen = 0;
  const WCHAR *wstr = nls_string(pool_idx, &wlen);
  return wide_to_utf8(wstr, wlen, dst, dst_len);
}

int nls_locale_name_utf8(const uint8_t *record, char *dst, size_t dst_len) {
  return nls_record_string_utf8(record, kLocaleNameOffset, dst, dst_len);
}

//===----------------------------------------------------------------------===//
// Indexed day/month name resolution
//===----------------------------------------------------------------------===//

// Resolve an indexed name (Mon-Sat, months) from the string pool.
static const WCHAR *resolve_indexed(const uint8_t *record, uint32_t lctype,
                                    uint32_t rec_field, int correction,
                                    int *out_len) {
  uint32_t base = read_u32(record + rec_field);
  if (base == 0)
    return nullptr;

  uint32_t table_idx = base + lctype * 2;
  auto *p = reinterpret_cast<const uint8_t *>(strings()) + table_idx * 2 -
            correction;
  uint32_t str_idx = read_u32(p);
  return nls_string(str_idx, out_len);
}

// Resolve Sunday (special case — stored at array base + 2 bytes).
static const WCHAR *resolve_sunday(const uint8_t *record, uint32_t rec_field,
                                   int *out_len) {
  uint32_t base = read_u32(record + rec_field);
  if (base == 0)
    return nullptr;

  auto *p = reinterpret_cast<const uint8_t *>(strings()) + base * 2 + 2;
  uint32_t str_idx = read_u32(p);
  return nls_string(str_idx, out_len);
}

int nls_day_month_name_utf8(const uint8_t *record, uint32_t lctype, char *dst,
                            size_t dst_len) {
  const WCHAR *wstr = nullptr;
  int wlen = 0;

  // Day names: SDAYNAME1-6 (0x2A-0x2F), SDAYNAME7/Sunday (0x30).
  if (lctype >= 0x2A && lctype <= 0x2F)
    wstr = resolve_indexed(record, lctype, kDayNamesBase, kDayNameCorrection,
                           &wlen);
  else if (lctype == 0x30)
    wstr = resolve_sunday(record, kDayNamesBase, &wlen);
  // Abbreviated day names: 0x31-0x36, Sunday 0x37.
  else if (lctype >= 0x31 && lctype <= 0x36)
    wstr = resolve_indexed(record, lctype, kAbbrevDayNamesBase,
                           kAbbrevDayCorrection, &wlen);
  else if (lctype == 0x37)
    wstr = resolve_sunday(record, kAbbrevDayNamesBase, &wlen);
  // Month names: 0x38-0x43.
  else if (lctype >= 0x38 && lctype <= 0x43)
    wstr = resolve_indexed(record, lctype, kMonthNamesBase, kMonthCorrection,
                           &wlen);
  // Abbreviated month names: 0x44-0x4F.
  else if (lctype >= 0x44 && lctype <= 0x4F)
    wstr = resolve_indexed(record, lctype, kAbbrevMonthNamesBase,
                           kAbbrevMonthCorrection, &wlen);
  else
    return 0;

  if (!wstr)
    return 0;
  return wide_to_utf8(wstr, wlen, dst, dst_len);
}

//===----------------------------------------------------------------------===//
// Calendar and pattern helpers
//===----------------------------------------------------------------------===//

namespace {

inline constexpr uint32_t kCalendarShortDateOffset = 0x04;
inline constexpr uint16_t kMaxCalendarType = 0x18;

static const WCHAR *resolve_pattern_array_base(uint32_t base, uint32_t index,
                                               int *out_len) {
  if (base == 0)
    return nullptr;

  auto *p = reinterpret_cast<const uint8_t *>(strings()) + base * 2 + 2 +
            index * sizeof(uint32_t);
  uint32_t pool_idx = read_u32(p);
  return nls_string(pool_idx, out_len);
}

static const WCHAR *resolve_record_pattern(const uint8_t *record,
                                           uint32_t field_offset,
                                           int *out_len) {
  return resolve_pattern_array_base(read_u32(record + field_offset), 0, out_len);
}

static const uint8_t *calendar_record_by_type(uint16_t calendar_type) {
  if (!calendar_records() || calendar_type == 0 || calendar_type > kMaxCalendarType)
    return nullptr;
  return calendar_records() +
         static_cast<uint32_t>(calendar_type - 1) * header()->calendar_record_size;
}

static const WCHAR *resolve_calendar_pattern(const uint8_t *calendar_record,
                                             uint32_t field_offset,
                                             int *out_len) {
  return resolve_pattern_array_base(read_u32(calendar_record + field_offset), 0,
                                    out_len);
}

static uint16_t calendar_list_count(const uint8_t *record) {
  uint32_t base = read_u32(record + kCalendarListOffset);
  if (base == 0)
    return 0;
  return read_u16(reinterpret_cast<const uint8_t *>(strings()) + base * 2);
}

static uint16_t calendar_list_entry(const uint8_t *record, uint16_t index) {
  uint32_t base = read_u32(record + kCalendarListOffset);
  uint16_t count = calendar_list_count(record);
  if (base == 0 || index >= count)
    return 0;
  return read_u16(reinterpret_cast<const uint8_t *>(strings()) + base * 2 + 2 +
                  index * sizeof(uint16_t));
}

static bool is_gregorian_calendar(uint16_t calendar_type) {
  switch (calendar_type) {
  case 1:  // CAL_GREGORIAN
  case 2:  // CAL_GREGORIAN_US
  case 9:  // CAL_GREGORIAN_ME_FRENCH
  case 10: // CAL_GREGORIAN_ARABIC
  case 11: // CAL_GREGORIAN_XLIT_ENGLISH
  case 12: // CAL_GREGORIAN_XLIT_FRENCH
    return true;
  default:
    return false;
  }
}

static uint16_t era_calendar_type(const uint8_t *record) {
  uint16_t count = calendar_list_count(record);
  if (count == 0)
    return 0;

  uint16_t default_calendar = calendar_list_entry(record, 0);
  if (!is_gregorian_calendar(default_calendar))
    return default_calendar;

  for (uint16_t i = 1; i < count; ++i) {
    uint16_t calendar_type = calendar_list_entry(record, i);
    if (!is_gregorian_calendar(calendar_type))
      return calendar_type;
  }

  return 0;
}

// Stream a single WCHAR as UTF-8 into a StringStream.
// Returns false only if the Unicode conversion itself fails (not overflow —
// StringStream tracks that internally via overflow()).
static bool stream_utf8_wchar(cpp::StringStream &ss, WCHAR ch) {
  char utf8[4];
  int n = windows::utf16_to_utf8(&ch, 1, utf8, sizeof(utf8));
  if (n < 0)
    return false;

  ss << cpp::string_view(utf8, static_cast<size_t>(n));
  return true;
}

// Stream a literal WCHAR, escaping '%' as "%%" for strftime patterns.
static bool stream_literal_wchar(cpp::StringStream &ss, WCHAR ch) {
  if (ch == u'%') {
    ss << "%%";
    return true;
  }
  return stream_utf8_wchar(ss, ch);
}

static bool is_pattern_letter(WCHAR ch) {
  switch (ch) {
  case u'd':
  case u'g':
  case u'h':
  case u'H':
  case u'M':
  case u'm':
  case u's':
  case u't':
  case u'y':
    return true;
  default:
    return false;
  }
}

static bool pattern_has_token(const WCHAR *pattern, int len, WCHAR token) {
  bool quoted = false;

  for (int i = 0; i < len; ++i) {
    WCHAR ch = pattern[i];
    if (ch == u'\'') {
      if (i + 1 < len && pattern[i + 1] == u'\'') {
        ++i;
        continue;
      }
      quoted = !quoted;
      continue;
    }

    if (!quoted && ch == token)
      return true;
  }

  return false;
}

static const char *translate_pattern_token(WCHAR ch, int count,
                                           bool use_era_years) {
  switch (ch) {
  case u'd':
    if (count == 3)
      return "%a";
    if (count >= 4)
      return "%A";
    return "%d";
  case u'g':
    return "%EC";
  case u'h':
    return "%I";
  case u'H':
    return "%H";
  case u'M':
    if (count == 3)
      return "%b";
    if (count >= 4)
      return "%B";
    return "%m";
  case u'm':
    return "%M";
  case u's':
    return "%S";
  case u't':
    return "%p";
  case u'y':
    if (use_era_years)
      return "%Ey";
    return (count == 2) ? "%y" : "%Y";
  default:
    return nullptr;
  }
}

static int translate_windows_pattern_utf8(const WCHAR *pattern, int len,
                                          bool use_era_years, char *dst,
                                          size_t dst_len) {
  if (!dst || dst_len == 0)
    return 0;

  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(dst, dst_len - 1));
  const bool has_explicit_era = pattern_has_token(pattern, len, u'g');

  for (int i = 0; i < len;) {
    WCHAR ch = pattern[i];

    if (ch == u'\'') {
      if (i + 1 < len && pattern[i + 1] == u'\'') {
        if (!stream_literal_wchar(ss, u'\''))
          return 0;
        i += 2;
        continue;
      }

      ++i;
      while (i < len) {
        if (pattern[i] == u'\'') {
          if (i + 1 < len && pattern[i + 1] == u'\'') {
            if (!stream_literal_wchar(ss, u'\''))
              return 0;
            i += 2;
            continue;
          }
          ++i;
          break;
        }

        if (!stream_literal_wchar(ss, pattern[i]))
          return 0;
        ++i;
      }

      continue;
    }

    if (is_pattern_letter(ch)) {
      int count = 1;
      while (i + count < len && pattern[i + count] == ch)
        ++count;

      const char *token =
          translate_pattern_token(ch, count, use_era_years || has_explicit_era);
      if (!token)
        return 0;
      ss << token;
      i += count;
      continue;
    }

    if (!stream_literal_wchar(ss, ch))
      return 0;
    ++i;
  }

  if (ss.overflow())
    return 0;

  dst[ss.str().size()] = '\0';
  return static_cast<int>(ss.str().size() + 1);
}

} // namespace

int nls_locale_pattern_utf8(const uint8_t *record, uint32_t field_offset,
                            char *dst, size_t dst_len) {
  int wlen = 0;
  const WCHAR *pattern = resolve_record_pattern(record, field_offset, &wlen);
  if (!pattern)
    return 0;

  return translate_windows_pattern_utf8(pattern, wlen, false, dst, dst_len);
}

int nls_era_date_pattern_utf8(const uint8_t *record, char *dst, size_t dst_len) {
  uint16_t calendar_type = era_calendar_type(record);
  const uint8_t *calendar_record = calendar_record_by_type(calendar_type);
  if (!calendar_record)
    return 0;

  int wlen = 0;
  const WCHAR *pattern =
      resolve_calendar_pattern(calendar_record, kCalendarShortDateOffset, &wlen);
  if (!pattern)
    return 0;

  return translate_windows_pattern_utf8(pattern, wlen, true, dst, dst_len);
}

int nls_alt_digits_utf8(const uint8_t *record, char *dst, size_t dst_len) {
  if (!dst || dst_len == 0)
    return 0;

  uint32_t base = read_u32(record + kSNativeDigitsOffset);
  if (base == 0)
    return 0;

  const uint8_t *pool_bytes = reinterpret_cast<const uint8_t *>(strings());
  WCHAR digits[10];
  bool all_ascii_digits = true;

  for (uint32_t i = 0; i < 10; ++i) {
    uint32_t pool_idx =
        read_u32(pool_bytes + (base + i * 2) * 2 + sizeof(uint16_t));
    int wlen = 0;
    const WCHAR *digit = nls_string(pool_idx, &wlen);
    if (wlen != 1)
      return 0;

    digits[i] = digit[0];
    if (digit[0] != static_cast<WCHAR>(u'0' + i))
      all_ascii_digits = false;
  }

  if (all_ascii_digits) {
    dst[0] = '\0';
    return 1;
  }

  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(dst, dst_len - 1));
  for (size_t i = 0; i < 10; ++i) {
    if (i != 0 && !stream_literal_wchar(ss, u';'))
      return 0;
    if (!stream_utf8_wchar(ss, digits[i]))
      return 0;
  }

  if (ss.overflow())
    return 0;

  dst[ss.str().size()] = '\0';
  return static_cast<int>(ss.str().size() + 1);
}

//===----------------------------------------------------------------------===//
// Locale name comparison (case-insensitive, _ treated as -)
//===----------------------------------------------------------------------===//

static int locale_name_cmp(const char *a, const WCHAR *b, int b_len) {
  for (int i = 0;; i++) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    WCHAR cb = (i < b_len) ? b[i] : 0;

    // ASCII case fold.
    if (ca >= 'A' && ca <= 'Z')
      ca |= 0x20;
    if (cb >= u'A' && cb <= u'Z')
      cb |= 0x20;
    // Treat _ as -.
    if (ca == '_')
      ca = '-';
    if (cb == u'_')
      cb = u'-';

    if (ca == 0 && cb == 0)
      return 0;
    if (ca == 0)
      return -1;
    if (cb == 0)
      return 1;
    if (ca != static_cast<unsigned char>(cb))
      return (ca < static_cast<unsigned char>(cb)) ? -1 : 1;
  }
}

//===----------------------------------------------------------------------===//
// Locale lookup
//===----------------------------------------------------------------------===//

// True if `cs` is a UTF-8 alias. Case-insensitive; the dash between "UTF"
// and "8" is optional. Accepts UTF8, UTF-8, utf8, utf-8, Utf-8, uTF8, …
// An empty codeset is treated as UTF-8 by the caller, not here.
static bool codeset_is_utf8(cpp::string_view cs) {
  if (cs.size() != 4 && cs.size() != 5)
    return false;
  if (internal::toupper(cs[0]) != 'U' || internal::toupper(cs[1]) != 'T' ||
      internal::toupper(cs[2]) != 'F')
    return false;
  if (cs.size() == 5 && cs[3] != '-')
    return false;
  return cs.back() == '8';
}

// Parses a POSIX locale name of the form "<lang>[.<codeset>][@<modifier>]".
// Writes the stripped name (language + territory, without codeset or modifier)
// into dst. Returns true iff the codeset, if specified, is a UTF-8 alias.
// An absent codeset is treated as UTF-8 (we support a UTF-8-only locale
// profile: any explicit non-UTF-8 codeset is rejected at setlocale time so
// callers never observe silent UTF-8 substitution).
static bool parse_and_validate_codeset(const char *name, char *dst,
                                       size_t dst_len) {
  cpp::string_view sv(name);
  const size_t dot = sv.find_first_of('.');
  const size_t at = sv.find_first_of('@');
  // Validate codeset if present. The codeset runs from just after '.' until
  // '@' (if it follows the dot) or end-of-string.
  if (dot != cpp::string_view::npos) {
    const size_t cs_end = (at != cpp::string_view::npos && at > dot)
                              ? at
                              : sv.size();
    cpp::string_view codeset = sv.substr(dot + 1, cs_end - dot - 1);
    if (!codeset.empty() && !codeset_is_utf8(codeset))
      return false;
  }
  const size_t end =
      cpp::min(dot == cpp::string_view::npos ? sv.size() : dot,
               at == cpp::string_view::npos ? sv.size() : at);
  cpp::string_view prefix = sv.substr(0, end);
  const size_t copy_len =
      prefix.size() < dst_len - 1 ? prefix.size() : dst_len - 1;
  __builtin_memcpy(dst, prefix.data(), copy_len);
  dst[copy_len] = '\0';
  return true;
}

const uint8_t *nls_find_locale(const char *name) {
  if (!name)
    return nullptr;

  // "C" and "POSIX" → C locale (caller handles this as nullptr).
  cpp::string_view name_sv(name);
  if (name_sv == "C" || name_sv == "POSIX")
    return nullptr;

  if (!nls_init())
    return nullptr;

  // "" → system default locale via LCID lookup.
  if (name_sv.empty())
    return nls_find_locale_by_lcid(g_pcb.nls.default_lcid);

  // Strip codeset/modifier, and reject non-UTF-8 codesets. We ship a UTF-8-
  // only locale profile; `nl_langinfo(CODESET)` will answer "UTF-8" for
  // every non-C locale, so accepting `.SJIS`/`.ISO-8859-*`/etc. would silently
  // lie to the caller.
  char stripped[64];
  if (!parse_and_validate_codeset(name, stripped, sizeof(stripped)))
    return nullptr;

  // Binary search the name table.
  int lo = 0;
  int hi = static_cast<int>(header()->name_count) - 1;

  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    NameEntry *e = &name_table()[mid];

    int slen = 0;
    const WCHAR *pool_name = nls_string(e->name_pool_index, &slen);

    int cmp = locale_name_cmp(stripped, pool_name, slen);
    if (cmp == 0)
      return records() +
             static_cast<uint32_t>(e->record_index) * header()->record_size;
    if (cmp < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }

  return nullptr;
}

const uint8_t *nls_find_locale_by_lcid(uint32_t lcid) {
  if (!blob())
    return nullptr;

  // Binary search the LCID table.
  int lo = 0;
  int hi = static_cast<int>(header()->lcid_count) - 1;

  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    uint32_t entry_lcid = lcid_table()[mid].lcid;

    if (lcid == entry_lcid)
      return records() + static_cast<uint32_t>(lcid_table()[mid].record_index) *
                             header()->record_size;
    if (lcid < entry_lcid)
      hi = mid - 1;
    else
      lo = mid + 1;
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// lconv population
//===----------------------------------------------------------------------===//

// Static buffers for lconv string fields. POSIX guarantees the returned
// pointer is valid until the next setlocale() call, so one set suffices.
// Max UTF-8 expansion: 3 bytes per BMP char. Typical locale strings are
// very short (1-20 chars).
static constexpr size_t kFieldBufSize = 64;

static char decimal_point_buf[kFieldBufSize];
static char thousands_sep_buf[kFieldBufSize];
static char currency_symbol_buf[kFieldBufSize];
static char mon_decimal_point_buf[kFieldBufSize];
static char mon_thousands_sep_buf[kFieldBufSize];
static char positive_sign_buf[kFieldBufSize];
static char negative_sign_buf[kFieldBufSize];
static char int_curr_symbol_buf[kFieldBufSize];

// Grouping buffers. POSIX grouping is a char array where each byte is a
// group width, terminated by 0 (repeat last) or CHAR_MAX (no further
// grouping). Max NLS grouping depth is small — 8 bytes is plenty.
static char grouping_buf[8];
static char mon_grouping_buf[8];

// Decode a WORD-array grouping descriptor from the NLS string pool into
// POSIX lconv grouping format. The blob stores: WORD count, then WORD
// widths[count]. POSIX wants: char widths[], terminated by 0 (meaning
// "repeat last group") or CHAR_MAX (no further grouping).
static char *decode_grouping(const uint8_t *record, uint32_t field_offset,
                             char *dst, size_t dst_len) {
  uint32_t base = read_u32(record + field_offset);
  if (base == 0 || dst_len < 2) {
    dst[0] = '\0';
    return dst;
  }

  const uint8_t *pool_bytes = reinterpret_cast<const uint8_t *>(strings());
  uint16_t count = read_u16(pool_bytes + base * 2);

  size_t pos = 0;
  for (uint16_t i = 0; i < count && pos < dst_len - 1; ++i) {
    uint16_t width = read_u16(pool_bytes + base * 2 + 2 + i * 2);
    if (width == 0)
      break;
    dst[pos++] = static_cast<char>(width);
  }

  // POSIX: trailing '\0' means repeat the last group size.
  dst[pos] = '\0';
  return dst;
}

// Positive currency layout derived from ICURRENCY (record+0x012).
//   ICURRENCY | p_cs_precedes | p_sep_by_space
//   0           1               0
//   1           0               0
//   2           1               1
//   3           0               1
struct PosLayout {
  char cs_precedes;
  char sep_by_space;
};
static constexpr PosLayout kPosLayout[] = {
    {1, 0}, {0, 0}, {1, 1}, {0, 1},
};

// Negative currency layout derived from INEGCURR (record+0x014).
struct NegLayout {
  char cs_precedes;
  char sep_by_space;
  char sign_posn;
  char pos_sign_posn;
};
static constexpr NegLayout kNegLayout[] = {
    {1, 0, 0, 3}, {1, 0, 3, 3}, {1, 0, 4, 4}, {1, 0, 2, 2},
    {0, 0, 0, 1}, {0, 0, 1, 1}, {0, 0, 3, 3}, {0, 0, 4, 4},
    {0, 1, 1, 1}, {1, 1, 3, 3}, {0, 1, 4, 4}, {1, 1, 2, 2},
    {1, 1, 4, 4}, {0, 1, 3, 3}, {1, 1, 0, 3}, {0, 1, 0, 1},
};

void nls_fill_lconv(const uint8_t *record, struct lconv *lc) {
  // String fields — read from verified NLS record offsets.
  nls_record_string_utf8(record, kSDecimalOffset, decimal_point_buf,
                         kFieldBufSize);
  lc->decimal_point = decimal_point_buf;

  nls_record_string_utf8(record, kSThousandOffset, thousands_sep_buf,
                         kFieldBufSize);
  lc->thousands_sep = thousands_sep_buf;

  nls_record_string_utf8(record, kSCurrencyOffset, currency_symbol_buf,
                         kFieldBufSize);
  lc->currency_symbol = currency_symbol_buf;

  nls_record_string_utf8(record, kSMonDecimalSepOffset, mon_decimal_point_buf,
                         kFieldBufSize);
  lc->mon_decimal_point = mon_decimal_point_buf;

  nls_record_string_utf8(record, kSMonThousandSepOffset, mon_thousands_sep_buf,
                         kFieldBufSize);
  lc->mon_thousands_sep = mon_thousands_sep_buf;

  nls_record_string_utf8(record, kSPositiveSignOffset, positive_sign_buf,
                         kFieldBufSize);
  lc->positive_sign = positive_sign_buf;

  nls_record_string_utf8(record, kSNegativeSignOffset, negative_sign_buf,
                         kFieldBufSize);
  lc->negative_sign = negative_sign_buf;

  nls_record_string_utf8(record, kSIntlSymbolOffset, int_curr_symbol_buf,
                         kFieldBufSize);
  lc->int_curr_symbol = int_curr_symbol_buf;

  // Grouping — decoded from WORD arrays in the NLS string pool.
  lc->grouping = decode_grouping(record, kSGroupingOffset, grouping_buf,
                                 sizeof(grouping_buf));
  lc->mon_grouping = decode_grouping(record, kSMonGroupingOffset,
                                     mon_grouping_buf, sizeof(mon_grouping_buf));

  // Fractional digits — WORD at record+0x010. IINTLCURRDIGITS aliases the
  // same offset, so both frac_digits and int_frac_digits get the same value.
  uint16_t icurrdigits = read_u16(record + kICurrDigitsOffset);
  lc->frac_digits = static_cast<char>(icurrdigits);
  lc->int_frac_digits = static_cast<char>(icurrdigits);

  // Positive currency layout — derived from ICURRENCY (record+0x012).
  uint16_t icurrency = read_u16(record + kICurrencyOffset);
  if (icurrency < 4) {
    lc->p_cs_precedes = kPosLayout[icurrency].cs_precedes;
    lc->p_sep_by_space = kPosLayout[icurrency].sep_by_space;
  } else {
    lc->p_cs_precedes = CHAR_MAX;
    lc->p_sep_by_space = CHAR_MAX;
  }

  // Negative currency layout — derived from INEGCURR (record+0x014).
  uint16_t inegcurr = read_u16(record + kINegCurrOffset);
  if (inegcurr < 16) {
    lc->n_cs_precedes = kNegLayout[inegcurr].cs_precedes;
    lc->n_sep_by_space = kNegLayout[inegcurr].sep_by_space;
    lc->n_sign_posn = kNegLayout[inegcurr].sign_posn;
    lc->p_sign_posn = kNegLayout[inegcurr].pos_sign_posn;
  } else {
    lc->n_cs_precedes = CHAR_MAX;
    lc->n_sep_by_space = CHAR_MAX;
    lc->n_sign_posn = CHAR_MAX;
    lc->p_sign_posn = CHAR_MAX;
  }

  // International currency layout mirrors the domestic layout.
  lc->int_p_cs_precedes = lc->p_cs_precedes;
  lc->int_n_cs_precedes = lc->n_cs_precedes;
  lc->int_p_sep_by_space = lc->p_sep_by_space;
  lc->int_n_sep_by_space = lc->n_sep_by_space;
  lc->int_p_sign_posn = lc->p_sign_posn;
  lc->int_n_sign_posn = lc->n_sign_posn;
}

} // namespace nls
} // namespace LIBC_NAMESPACE_DECL
