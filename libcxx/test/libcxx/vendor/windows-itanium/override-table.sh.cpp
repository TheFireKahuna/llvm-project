//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: c++03, c++11, c++14

// Every overridable default of the shared library begins by reading its slot
// in the table that the executable carries in its header page, and transfers
// to the function the slot names: from the executable's calls, which import
// the default, from the library's own calls, and through a slot that forwards
// to another default's replacement. The executable's address of the default
// stays the library's entry. The library checks the table against its anchor
// when it loads, and the executable checks it again in its first TLS callback
// (__ovr_check, which the program's link names), so a table that no longer
// matches its link fails fast. A helper writes the table that the program's
// link would, from the anchor that the program defines here.

// RUN: %{cxx} %{flags} %{compile_flags} -DTOOL %s %{link_flags} -o %{temp}/tool.exe
// RUN: %{exec} %{temp}/tool.exe stub %{temp}/stub.bin
// RUN: %{cxx} %{flags} %{compile_flags} %s %{link_flags} -Wl,-stub:%{temp}/stub.bin -Wl,-include:__ovr_check -o %{temp}/main.exe
// RUN: %{exec} %{temp}/tool.exe table %{temp}/main.exe
// RUN: %{exec} %{temp}/main.exe > %{temp}/main.txt
// RUN: grep -q replaced %{temp}/main.txt

// A slot moved 16 bytes into its function: the library's check fails before
// main, here alone, since the executable names no anchor of its own.
// RUN: %{cxx} %{flags} %{compile_flags} -DNO_ANCHOR %s %{link_flags} -Wl,-stub:%{temp}/stub.bin -o %{temp}/moved.exe
// RUN: %{exec} %{temp}/tool.exe moved %{temp}/moved.exe
// RUN: not %{exec} %{temp}/moved.exe > %{temp}/moved.txt
// RUN: not grep -q main %{temp}/moved.txt

// The magic erased: the library finds the anchor's section, which the table
// would match, and fails before any image that imports it initializes.
// RUN: cp %{temp}/main.exe %{temp}/erased.exe
// RUN: %{exec} %{temp}/tool.exe erased %{temp}/erased.exe
// RUN: not %{exec} %{temp}/erased.exe > %{temp}/erased.txt
// RUN: not grep -q main %{temp}/erased.txt

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr std::uint32_t Magic = 0x7B3AC2D5;

#ifdef TOOL

#  include <vector>

constexpr std::uint32_t TagBase = 0xC5E1A7F3;

static std::uint32_t slotOffset(unsigned k) { return k < 8 ? 0x40 + 8 * k : 0x98 + 8 * (k - 8); }

template <class T>
static T get(const std::vector<unsigned char>& f, std::size_t at) {
  T v;
  std::memcpy(&v, f.data() + at, sizeof(T));
  return v;
}

template <class T>
static void put(std::vector<unsigned char>& f, std::size_t at, T v) {
  std::memcpy(f.data() + at, &v, sizeof(T));
}

int main(int, char** argv) {
  const char* mode = argv[1];
  std::vector<unsigned char> f;
  if (std::strcmp(mode, "stub") == 0) {
    // An MZ header, and zeros up to the table's end, before the PE header.
    f.assign(0x120, 0);
    f[0] = 'M';
    f[1] = 'Z';
  } else {
    std::FILE* in = std::fopen(argv[2], "rb");
    if (!in)
      return 1;
    std::fseek(in, 0, SEEK_END);
    f.resize(std::ftell(in));
    std::fseek(in, 0, SEEK_SET);
    if (std::fread(f.data(), 1, f.size(), in) != f.size())
      return 1;
    std::fclose(in);

    std::uint32_t pe        = get<std::uint32_t>(f, 0x3C);
    std::uint16_t sections  = get<std::uint16_t>(f, pe + 6);
    std::uint16_t optional  = get<std::uint16_t>(f, pe + 20);
    std::uint64_t imageBase = get<std::uint64_t>(f, pe + 24 + 24);
    std::size_t header      = pe + 24 + optional;
    for (unsigned i = 0; i != sections; ++i, header += 40) {
      if (std::memcmp(f.data() + header, ".ovra\0\0\0", 8) != 0)
        continue;
      std::uint32_t rva   = get<std::uint32_t>(f, header + 12);
      std::uint32_t raw   = get<std::uint32_t>(f, header + 20);
      std::uint16_t count = get<std::uint16_t>(f, raw + 6);
      for (unsigned k = 0; k != count; ++k)
        if (std::uint64_t entry = get<std::uint64_t>(f, raw + 16 + 8 * k)) {
          put<std::uint32_t>(f, slotOffset(k), TagBase ^ k);
          put<std::uint32_t>(f, slotOffset(k) + 4, std::uint32_t(entry - imageBase));
        }
      put<std::uint32_t>(f, 0x80, Magic);
      put<std::uint16_t>(f, 0x84, 1);
      put<std::uint16_t>(f, 0x86, count);
      put<std::uint32_t>(f, 0x88, rva);
    }
    if (std::strcmp(mode, "moved") == 0)
      put<std::uint32_t>(f, 0x44, get<std::uint32_t>(f, 0x44) + 16);
    if (std::strcmp(mode, "erased") == 0)
      put<std::uint32_t>(f, 0x80, 0);
  }
  std::FILE* out = std::fopen(argv[2], "wb");
  if (!out || std::fwrite(f.data(), 1, f.size(), out) != f.size())
    return 1;
  std::fclose(out);
  return 0;
}

#else

#  include <new>
#  include <string>
#  include <system_error>
#  include <windows.h>

constexpr unsigned Slots = 24;

static int replaced;

static void* replacementNew(std::size_t size) {
  ++replaced;
  return std::malloc(size);
}

// What the program's link writes beside the table: the slots that name a
// function. Slot 0, operator new(size_t), is replaced, and slot 1,
// operator new[](size_t), forwards to its replacement.
struct Anchor {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t count;
  std::uint32_t mask;
  std::uint32_t reserved;
  const void* entry[Slots];
};
#  ifdef NO_ANCHOR
#    define ANCHOR another_anchor
#  else
#    define ANCHOR __ovr_anchor
#  endif
extern "C" [[gnu::section(".ovra"), gnu::used]] const Anchor ANCHOR = {
    Magic,
    1,
    Slots,
    1,
    0,
    {reinterpret_cast<const void*>(&replacementNew), reinterpret_cast<const void*>(&replacementNew)}};

int main() {
  std::puts("main");
  std::fflush(stdout);
  ::operator delete(::operator new(16));
  if (replaced != 1)
    return 1;
  ::operator delete[](::operator new[](16));
  if (replaced != 2)
    return 2;
  // A slot that names nothing keeps the default.
  ::operator delete(::operator new(16, std::align_val_t(64)), std::align_val_t(64));
  if (replaced != 2)
    return 3;
  // The library allocates the message, which is longer than a short string.
  if (std::generic_category().message(EPERM).size() <= 22 || replaced != 3)
    return 4;
  void* (*entry)(std::size_t) = &::operator new;
  if (reinterpret_cast<std::uintptr_t>(entry) !=
      reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"libc++.dll"), "_Znwy")))
    return 5;
  std::puts("replaced");
  return 0;
}

#endif
