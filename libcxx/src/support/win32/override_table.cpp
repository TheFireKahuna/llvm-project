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
// slots that name a replacement rather than a forwarded one, a reserved u32,
// then for each slot the address that its table entry names, or null.
constexpr std::uint32_t __anchor_entries = 16;

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

// An executable whose thread-local data holds the addresses of imports names,
// as {tag u32, rva u32} in the reserved words of its MZ header at 0x1C, the
// linker's copy of those words into the main thread's block, which the loader
// filled from the template before it bound them. A replacement of a default
// may read them, and runs as soon as an image that imports this library
// allocates, before the executable's own TLS callback runs the copy; so the
// copy runs here first. It acts only while the process initializes, and leaves
// a word that no longer holds its lookup entry alone, so a second run does
// nothing.
constexpr std::uint32_t __tls_copy_offset = 0x1C;
constexpr std::uint32_t __tls_copy_tag    = 0x9E3779B9;

// The copy is the linker's TLS callback, which carries no kcfi type.
__attribute__((__no_sanitize__("kcfi"))) void
__copy_executable_thread_locals(const unsigned char* __base, std::uint32_t __image_size) {
  if (__read<std::uint32_t>(__base + __tls_copy_offset) != __tls_copy_tag)
    return;
  std::uint32_t __rva = __read<std::uint32_t>(__base + __tls_copy_offset + 4);
  if (__rva == 0 || __rva >= __image_size)
    __fail();
  using __tls_callback   = void (*)(void*, unsigned long, void*);
  unsigned char* __image = const_cast<unsigned char*>(__base);
  reinterpret_cast<__tls_callback>(__image + __rva)(__image, 1, nullptr);
}

void __check_override_table(const unsigned char* __base) {
  // Without the magic, nothing tells an erased table from a program that
  // never had one.
  if (__read<std::uint32_t>(__base + 0x80) != __table_magic)
    return;

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

  // The extension serves only the defaults of user libraries, which read and
  // check their own entries.
  const unsigned char* __anchor = __base + __anchor_rva;
  std::uint32_t __mask          = __read<std::uint32_t>(__anchor + 8);
  if (__read<std::uint32_t>(__anchor) != __table_magic || __read<std::uint16_t>(__anchor + 4) != __version ||
      __read<std::uint16_t>(__anchor + 6) != __count || (__count < 32 && (__mask >> __count) != 0))
    __fail();

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

  __copy_executable_thread_locals(__base, __image_size);
}

// Before the library's other initializers, which allocate.
_LIBCPP_DIAGNOSTIC_PUSH
_LIBCPP_CLANG_DIAGNOSTIC_IGNORED("-Wprio-ctor-dtor")
[[gnu::constructor(99)]] void __check_executable_override_table() { __check_override_table(__executable()); }
_LIBCPP_DIAGNOSTIC_POP

} // namespace

#endif // _LIBCPP_HAS_OVERRIDE_TABLE
