//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: c++03, c++11, c++14

// Each overridable default of the library records its slot in the executable's
// override table, and the slot of the default it is specified as calling, and
// the library's link writes them into its import library, from which the
// program's link writes the table. Slots never change. A program that replaces
// some defaults finds each in its slot, and each default that calls a replaced
// one forwarded to it, through a thunk where the replaced one takes its
// arguments without the second.

// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} -o %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe

#include <__verbose_abort>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "test_macros.h"

extern "C" char __ImageBase;

void* operator new(std::size_t n) { return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t) { return std::malloc(n ? n : 1); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void std::__libcpp_verbose_abort(char const*, ...) noexcept { std::abort(); }

namespace {
const unsigned char* base() { return reinterpret_cast<const unsigned char*>(&__ImageBase); }

std::uint32_t slot_offset(unsigned k) { return k < 8 ? 0x40 + 8 * k : 0x98 + 8 * (k - 8); }

// The function slot k names, after checking that it holds its tag, or null.
const void* slot(unsigned k) {
  std::uint32_t tag, rva;
  std::memcpy(&tag, base() + slot_offset(k), 4);
  std::memcpy(&rva, base() + slot_offset(k) + 4, 4);
  if (tag != (0xC5E1A7F3u ^ k) || rva == 0)
    return nullptr;
  return base() + rva;
}

template <class F>
const void* address(F* f) {
  return reinterpret_cast<const void*>(f);
}
} // namespace

int main(int, char**) {
  const void* new_ = address(static_cast<void* (*)(std::size_t)>(&::operator new));
  const void* delete_ = address(static_cast<void (*)(void*) noexcept>(&::operator delete));
  const void* new_aligned = address(static_cast<void* (*)(std::size_t, std::align_val_t)>(&::operator new));
  const void* delete_aligned =
      address(static_cast<void (*)(void*, std::align_val_t) noexcept>(&::operator delete));

  // Replaced: new 0, delete 2, aligned new 6, aligned delete 7, nothrow new 12,
  // nothrow delete 16, __libcpp_verbose_abort 20.
  assert(slot(0) == new_);
  assert(slot(2) == delete_);
  assert(slot(6) == new_aligned);
  assert(slot(7) == delete_aligned);
  assert(slot(12) == address(static_cast<void* (*)(std::size_t, const std::nothrow_t&) noexcept>(&::operator new)));
  assert(slot(16) == address(static_cast<void (*)(void*, const std::nothrow_t&) noexcept>(&::operator delete)));
  assert(slot(20) == address(&std::__libcpp_verbose_abort));

  // Forwarded: new[] 1 to new; sized delete 3, delete[] 4 and sized delete[] 5
  // to delete; aligned new[] 8 to aligned new; aligned delete[] 10 to aligned
  // delete; sized aligned delete 9 and sized aligned delete[] 11 to aligned
  // delete through a thunk that drops the size.
  assert(slot(1) == new_);
  assert(slot(3) == delete_);
  assert(slot(4) == delete_);
  assert(slot(5) == delete_);
  assert(slot(8) == new_aligned);
  assert(slot(10) == delete_aligned);
  assert(slot(9) != nullptr && slot(9) != delete_aligned);
  assert(slot(11) != nullptr && slot(11) != delete_aligned);

  // Not replaced: the other nothrow forms.
  assert(slot(13) == nullptr);
  assert(slot(17) == nullptr);
  return 0;
}
