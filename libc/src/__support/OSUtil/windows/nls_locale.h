//===-- NLS locale blob parser for Windows -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Parses the Windows NLS locale blob mapped by NtInitializeNlsFiles.
// Fork-safe: the blob is a read-only kernel section view, no kernel32 state.
//
// The blob contains all locale metadata: decimal separators, currency symbols,
// day/month names, language names, etc. KernelBase's GetLocaleInfoEx reads from
// the same blob — we replicate its logic without the kernel32 dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_LOCALE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_LOCALE_H

#include "src/__support/macros/config.h"

#include <stddef.h>
#include <stdint.h>

// Forward-declare WCHAR to avoid pulling in ntdll.h in every consumer.
// Always 16-bit UTF-16 regardless of wchar_t width (see ntdll.h).
using WCHAR = char16_t;

struct lconv;

namespace LIBC_NAMESPACE_DECL {
namespace nls {

//===----------------------------------------------------------------------===//
// NLS Blob Structures
//===----------------------------------------------------------------------===//

struct BlobHeader {
  uint32_t header_size;    // Always 0x20.
  uint32_t reserved[3];
  uint32_t locale_section; // Byte offset to locale data section.
  uint32_t section2;       // Codepage data.
  uint32_t section3;       // Sort data.
  uint32_t section4;       // Casing data.
};

struct LocaleHeader {
  uint32_t default_lcid;      // 0x00
  uint8_t pad04[0x12];
  uint16_t lcid_count;        // 0x16
  uint16_t lcid_count2;       // 0x18
  uint16_t record_size;       // 0x1A: bytes per locale record (328).
  uint32_t records_offset;    // 0x1C: relative to section base.
  uint16_t name_count;        // 0x20
  uint8_t pad22[2];
  uint32_t lcid_table_offset; // 0x24
  uint32_t name_table_offset; // 0x28
  uint8_t pad2c[4];
  uint8_t pad30[2];
  uint16_t calendar_record_size; // 0x32
  uint32_t calendar_records_offset; // 0x34: relative to section base.
  uint32_t string_pool_offset; // 0x38
};

struct LcidEntry {
  uint32_t lcid;
  uint16_t record_index;
  uint16_t flags;
};

struct NameEntry {
  uint16_t name_pool_index;
  uint16_t record_index;
  uint32_t lcid;
};

//===----------------------------------------------------------------------===//
// Locale record field offsets (verified against GetLocaleInfoEx)
//===----------------------------------------------------------------------===//

// Direct scalar fields (WORD values at these record offsets).
inline constexpr uint32_t kICurrDigitsOffset = 0x010;
inline constexpr uint32_t kICurrencyOffset = 0x012;
inline constexpr uint32_t kINegCurrOffset = 0x014;

// Grouping array fields (DWORD pool base at these record offsets).
inline constexpr uint32_t kSGroupingOffset = 0x024;
inline constexpr uint32_t kSMonGroupingOffset = 0x028;

// String fields — DWORD pool indices at these record offsets.
inline constexpr uint32_t kLocaleNameOffset = 0x000;
inline constexpr uint32_t kSListOffset = 0x02C;
inline constexpr uint32_t kSDecimalOffset = 0x030;
inline constexpr uint32_t kSThousandOffset = 0x034;
inline constexpr uint32_t kSCurrencyOffset = 0x038;
inline constexpr uint32_t kSMonDecimalSepOffset = 0x03C;
inline constexpr uint32_t kSMonThousandSepOffset = 0x040;
inline constexpr uint32_t kSPositiveSignOffset = 0x044;
inline constexpr uint32_t kSNegativeSignOffset = 0x048;
inline constexpr uint32_t kS1159Offset = 0x04C; // AM designator
inline constexpr uint32_t kS2359Offset = 0x050; // PM designator
inline constexpr uint32_t kSNativeDigitsOffset = 0x054;
inline constexpr uint32_t kSTimeFormatOffset = 0x058;
inline constexpr uint32_t kSShortDateOffset = 0x05C;
inline constexpr uint32_t kSLongDateOffset = 0x060;
inline constexpr uint32_t kSAbbrevLangNameOffset = 0x080;
inline constexpr uint32_t kSIso639LangNameOffset = 0x084;
inline constexpr uint32_t kSEnglishLanguageNameOffset = 0x088;
inline constexpr uint32_t kSNativeLanguageNameOffset = 0x08C;
inline constexpr uint32_t kSEnglishCountryNameOffset = 0x090;
inline constexpr uint32_t kSNativeCountryNameOffset = 0x094;
inline constexpr uint32_t kSAbbrevCtryNameOffset = 0x098;
inline constexpr uint32_t kSIso3166CtryNameOffset = 0x09C;
inline constexpr uint32_t kSIntlSymbolOffset = 0x0A0;
inline constexpr uint32_t kSEngCurrNameOffset = 0x0A4;
inline constexpr uint32_t kSNativeCurrNameOffset = 0x0A8;
inline constexpr uint32_t kSParentOffset = 0x0B8;

// Day/month name array bases (DWORD at record offset).
inline constexpr uint32_t kDayNamesBase = 0x0BC;
inline constexpr uint32_t kAbbrevDayNamesBase = 0x0C0;
inline constexpr uint32_t kMonthNamesBase = 0x0C4;
inline constexpr uint32_t kAbbrevMonthNamesBase = 0x0C8;

// Correction factors for indexed day/month name resolution.
inline constexpr int kDayNameCorrection = 0xA2;
inline constexpr int kAbbrevDayCorrection = 0xBE;
inline constexpr int kMonthCorrection = 0xDE;
inline constexpr int kAbbrevMonthCorrection = 0x10E;

inline constexpr uint32_t kSNameOffset = 0x114;
inline constexpr uint32_t kSShortTimeOffset = 0x118;
inline constexpr uint32_t kCalendarListOffset = 0x07C;

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

// Initialize NLS blob mapping. Thread-safe (callonce). Returns true on success.
bool nls_init();

// Find a locale record by name. Accepts:
//   "C" / "POSIX"    → returns nullptr (caller uses C locale)
//   ""               → system default locale
//   "en-US"          → direct NLS name lookup
//   "en_US.UTF-8"    → strips codeset, _ → -, then lookup
// Returns pointer into the read-only NLS blob, or nullptr for C locale.
const uint8_t *nls_find_locale(const char *name);

// Find a locale record by LCID (binary search on LCID table).
const uint8_t *nls_find_locale_by_lcid(uint32_t lcid);

// Read a length-prefixed UTF-16 string from the string pool.
// Returns pointer to the string data (past the length word).
// Sets *out_len to the character count (0 for empty strings).
const WCHAR *nls_string(uint32_t pool_index, int *out_len);

// Read a string pool index from a locale record field and convert to UTF-8.
// Returns the number of bytes written (including NUL), or 0 on failure.
// dst must be at least dst_len bytes.
int nls_record_string_utf8(const uint8_t *record, uint32_t field_offset,
                           char *dst, size_t dst_len);

// Resolve an indexed day/month name to UTF-8.
// lctype: the LCTYPE value (0x2A-0x4F).
// Returns bytes written (including NUL), or 0 on failure.
int nls_day_month_name_utf8(const uint8_t *record, uint32_t lctype, char *dst,
                            size_t dst_len);

// Resolve a locale date/time pattern field and translate it to a POSIX-style
// strftime pattern in UTF-8.
int nls_locale_pattern_utf8(const uint8_t *record, uint32_t field_offset,
                            char *dst, size_t dst_len);

// Resolve the locale's preferred era-aware short date pattern and translate it
// to a POSIX-style strftime pattern in UTF-8. Returns 0 when the locale has no
// non-Gregorian era calendar.
int nls_era_date_pattern_utf8(const uint8_t *record, char *dst, size_t dst_len);

// Resolve the locale's native digit table and format it as a semicolon-
// separated UTF-8 ALT_DIGITS list. Returns 1 with an empty string when the
// locale uses ASCII digits.
int nls_alt_digits_utf8(const uint8_t *record, char *dst, size_t dst_len);

// Populate an lconv struct from an NLS locale record.
// All string and integer fields are read from the blob.
void nls_fill_lconv(const uint8_t *record, struct lconv *lc);

// Get the string pool base pointer (for direct UTF-16 access).
const WCHAR *nls_string_pool();

// Get the locale name from a record as UTF-8.
// Returns bytes written (including NUL), or 0 on failure.
int nls_locale_name_utf8(const uint8_t *record, char *dst, size_t dst_len);

} // namespace nls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NLS_LOCALE_H
