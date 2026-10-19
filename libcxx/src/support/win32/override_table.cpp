//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// When the shared library loads, before any image that imports it initializes,
// it checks the override table of the running executable against the anchor
// that the executable's link wrote beside it, and fails fast when the two
// differ: a table that no longer matches its link would send a default to code
// that the link did not choose. The check only reads, and only read-only
// memory of the executable's image.

#include <__config>
#include <cstdint>
#include <cstring>

#include "include/overridable_function.h"

#if _LIBCPP_HAS_OVERRIDE_TABLE

namespace {

// The table's version-1 layout, in the executable's header page: slot k is
// {tag u32, rva u32} at 0x40 + 8k for the eight slots of the first cache line
// and at 0x98 + 8(k - 8) after the header, which holds the magic u32 and the
// version and slot count u16 at 0x80, the anchor's RVA u32 at 0x88, the
// extension's RVA u32 at 0x8C, and a reserved u64 at 0x90.
constexpr std::uint32_t __table_magic = 0x7B3AC2D5;
constexpr std::uint32_t __tag_base    = 0xC5E1A7F3;

// The anchor: the magic, version and slot count of its table, the mask of the
// slots that name a replacement rather than a forwarded one, the number of the
// block's entries, then for each slot the address that its table entry names,
// or null, then for each entry of the block, in the block's order, its key and
// the address it names.
constexpr std::uint32_t __anchor_entries = 16;

// The block of the defaults replaced by name, which the header's u32 at 0x8C
// names: the magic u32, the version and the number of entries u16, the
// overflow list's RVA and length u32, and two reserved u64 at 0x7E0, then 64
// buckets of {key u64, rva u32, info u32} from 0x800. The overflow list has the
// buckets' layout.
constexpr std::uint32_t __block_offset   = 0x7E0;
constexpr std::uint32_t __buckets_offset = 0x800;
constexpr std::uint32_t __block_end      = 0xC00;
constexpr std::uint32_t __block_magic    = 0x2F1B8E57;

std::uint32_t __slot_offset(std::uint32_t __k) { return __k < 8 ? 0x40 + 8 * __k : 0x98 + 8 * (__k - 8); }

template <class _Tp>
_Tp __read(const unsigned char* __p) {
  _Tp __v;
  std::memcpy(&__v, __p, sizeof(_Tp));
  return __v;
}

[[noreturn]] void __fail() {
  // __fastfail(FAST_FAIL_FATAL_APP_EXIT): the process ends with
  // STATUS_STACK_BUFFER_OVERRUN, and no handler of the process runs.
#  if defined(__x86_64__)
  __asm__ volatile("int $0x29" : : "c"(7) : "memory");
#  elif defined(__aarch64__)
  register unsigned long __code __asm__("x0") = 7;
  __asm__ volatile("brk #0xf003" : : "r"(__code) : "memory");
#  endif
  __builtin_unreachable();
}

// PEB->ImageBaseAddress, through TEB->ProcessEnvironmentBlock.
const unsigned char* __executable() {
  const unsigned char* __peb;
#  if defined(__x86_64__)
  __asm__("movq %%gs:0x60, %0" : "=r"(__peb));
#  elif defined(__aarch64__)
  __asm__("ldr %0, [x18, #0x60]" : "=r"(__peb));
#  endif
  return __read<const unsigned char*>(__peb + 0x10);
}

// Whether the executable has the read-only section that a replacing link puts
// the anchor in, .ovra, beginning with the table's magic and version: a table
// that a rewrite of the header page erased leaves it in place. The walk reads
// the header page and the section only.
bool __has_anchor_section(const unsigned char* __base) {
  std::uint32_t __lfanew = __read<std::uint32_t>(__base + 0x3C);
  if (__lfanew > 0x1000 - 24)
    return false;
  const unsigned char* __nt  = __base + __lfanew;
  std::uint32_t __image_size = __read<std::uint32_t>(__nt + 0x50);
  std::uint32_t __sections   = __read<std::uint16_t>(__nt + 6);
  std::uint32_t __first      = __lfanew + 24 + __read<std::uint16_t>(__nt + 20);
  for (std::uint32_t __i = 0; __i != __sections && __first + 40 * (__i + 1) <= 0x1000; ++__i) {
    const unsigned char* __header = __base + __first + 40 * __i;
    std::uint32_t __rva           = __read<std::uint32_t>(__header + 12);
    if (std::memcmp(__header, ".ovra\0\0\0", 8) != 0 || __read<std::uint32_t>(__header + 8) < 8 ||
        std::uint64_t(__rva) + 8 > __image_size)
      continue;
    return __read<std::uint32_t>(__base + __rva) == __table_magic &&
           __read<std::uint16_t>(__base + __rva + 4) == _LIBCPP_OVERRIDE_TABLE_VERSION;
  }
  return false;
}

void __check_override_table(const unsigned char* __base) {
  // Without the magic, only the anchor's section tells an erased table from a
  // program that never had one.
  if (__read<std::uint32_t>(__base + 0x80) != __table_magic) {
    if (__has_anchor_section(__base))
      __fail();
    return;
  }

  std::uint16_t __version = __read<std::uint16_t>(__base + 0x84);
  std::uint16_t __count   = __read<std::uint16_t>(__base + 0x86);
  if (__version != _LIBCPP_OVERRIDE_TABLE_VERSION || __count < 8 || __read<std::uint64_t>(__base + 0x90) != 0)
    __fail();

  // The PE header follows the table, within the image.
  std::uint32_t __end    = __slot_offset(__count - 1) + 8;
  std::uint32_t __lfanew = __read<std::uint32_t>(__base + 0x3C);
  if (__end > 0x1000 || __lfanew < __end || __lfanew > 0x1000)
    __fail();
  std::uint32_t __image_size = __read<std::uint32_t>(__base + __lfanew + 0x50);
  std::uint32_t __anchor_rva = __read<std::uint32_t>(__base + 0x88);
  std::uint64_t __anchor_end = std::uint64_t(__anchor_rva) + __anchor_entries + 8 * std::uint64_t(__count);
  if (__anchor_rva == 0 || __anchor_end > __image_size)
    __fail();

  const unsigned char* __anchor = __base + __anchor_rva;
  std::uint32_t __mask          = __read<std::uint32_t>(__anchor + 8);
  std::uint32_t __named         = __read<std::uint32_t>(__anchor + 12);
  if (__read<std::uint32_t>(__anchor) != __table_magic || __read<std::uint16_t>(__anchor + 4) != __version ||
      __read<std::uint16_t>(__anchor + 6) != __count || (__count < 32 && (__mask >> __count) != 0) ||
      __anchor_end + 16 * std::uint64_t(__named) > __image_size)
    __fail();

  // Each entry of the block, in the block's order, against the anchor's.
  std::uint32_t __block = __read<std::uint32_t>(__base + 0x8C);
  if (__block == 0) {
    if (__named != 0)
      __fail();
  } else {
    std::uint32_t __overflow       = __read<std::uint32_t>(__base + __block_offset + 8);
    std::uint32_t __overflow_count = __read<std::uint32_t>(__base + __block_offset + 12);
    if (__block != __block_offset || __lfanew > __block_offset ||
        __read<std::uint32_t>(__base + __lfanew + 0x54) < __block_end ||
        __read<std::uint32_t>(__base + __block_offset) != __block_magic ||
        __read<std::uint16_t>(__base + __block_offset + 4) != __version ||
        __read<std::uint16_t>(__base + __block_offset + 6) != __named ||
        __read<std::uint64_t>(__base + __block_offset + 16) != 0 ||
        __read<std::uint64_t>(__base + __block_offset + 24) != 0 ||
        std::uint64_t(__overflow) + 16 * std::uint64_t(__overflow_count) > __image_size ||
        (__overflow_count != 0 && __overflow == 0))
      __fail();
    const unsigned char* __entry = __anchor + __anchor_end - __anchor_rva;
    std::uint32_t __seen         = 0;
    auto __check                 = [&](const unsigned char* __e) {
      std::uint32_t __rva = __read<std::uint32_t>(__e + 8);
      if (__seen == __named || __rva == 0 || __read<std::uint64_t>(__e) != __read<std::uint64_t>(__entry) ||
          __read<const unsigned char*>(__entry + 8) != __base + __rva)
        __fail();
      __entry += 16;
      ++__seen;
    };
    for (std::uint32_t __b = 0; __b != 64; ++__b) {
      const unsigned char* __e = __base + __buckets_offset + 16 * __b;
      if (__read<std::uint64_t>(__e) != 0)
        __check(__e);
      else if (__read<std::uint64_t>(__e + 8) != 0)
        __fail();
    }
    for (std::uint32_t __i = 0; __i != __overflow_count; ++__i)
      __check(__base + __overflow + 16 * __i);
    if (__seen != __named)
      __fail();
  }

  for (std::uint32_t __k = 0; __k != __count; ++__k) {
    const unsigned char* __slot  = __base + __slot_offset(__k);
    std::uint32_t __tag          = __read<std::uint32_t>(__slot);
    std::uint32_t __rva          = __read<std::uint32_t>(__slot + 4);
    const unsigned char* __entry = __read<const unsigned char*>(__anchor + __anchor_entries + 8 * __k);
    if (__tag == (__tag_base ^ __k) && __rva != 0) {
      if (__entry != __base + __rva)
        __fail();
    } else if (__tag != 0 || __rva != 0 || __entry != nullptr || (__k < 32 && (__mask >> __k) & 1)) {
      __fail();
    }
  }
}

// Before the library's other initializers, which allocate.
_LIBCPP_DIAGNOSTIC_PUSH
_LIBCPP_CLANG_DIAGNOSTIC_IGNORED("-Wprio-ctor-dtor")
[[gnu::constructor(99)]] void __check_executable_override_table() { __check_override_table(__executable()); }
_LIBCPP_DIAGNOSTIC_POP

} // namespace

#endif // _LIBCPP_HAS_OVERRIDE_TABLE
