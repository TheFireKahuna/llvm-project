//===-- override_check.cpp - The executable's override table check --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The link of an executable that replaces an overridable default writes the
// override table into its header page and an anchor beside it, the second
// copy of every slot, and names __ovr_check, which loads this file. Its TLS
// callback is the executable's first, so it compares the two before any code
// of the executable runs, and fails fast when they differ: a table that no
// longer matches its link would send a default to code that the link did not
// choose. An executable that replaces nothing links neither this file nor the
// TLS directory that it brings.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

// The anchor, which the link that names __ovr_check defines. Hidden, so that
// no import library offers it.
extern "C" __attribute__((visibility("hidden")))
const unsigned char __ovr_anchor[];

namespace {

template <class T> T readAt(const unsigned char *P) {
  T V;
  __builtin_memcpy(&V, P, sizeof(T));
  return V;
}

uint32_t slotOffset(uint32_t K) {
  return K < 8 ? 0x40 + 8 * K : 0x98 + 8 * (K - 8);
}

// The table's version-1 layout: slot K is {tag u32, rva u32} at 0x40 + 8K for
// the eight slots of the first cache line and at 0x98 + 8(K - 8) after the
// header, which holds the magic u32 and the version and slot count u16 at
// 0x80, the anchor's RVA u32 at 0x88 and a reserved u64 at 0x90. The anchor
// holds the magic, version and slot count, the mask of the slots that name a
// replacement rather than a forwarded one, a reserved u32, then for each slot
// the address its table entry names, or null. This only reads.
void checkOverrideTable() {
  constexpr uint32_t Magic = 0x7B3AC2D5;
  constexpr uint32_t TagBase = 0xC5E1A7F3;
  const auto *Base = reinterpret_cast<const unsigned char *>(&__ImageBase);
  const unsigned char *Anchor = __ovr_anchor;
  uint16_t Count = readAt<uint16_t>(Base + 0x86);
  if (readAt<uint32_t>(Base + 0x80) != Magic ||
      readAt<uint16_t>(Base + 0x84) != 1 || Count < 8 ||
      readAt<uint64_t>(Base + 0x90) != 0 ||
      Base + readAt<uint32_t>(Base + 0x88) != Anchor ||
      readAt<uint32_t>(Base + 0x3C) < slotOffset(Count - 1) + 8)
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  uint32_t Mask = readAt<uint32_t>(Anchor + 8);
  if (readAt<uint32_t>(Anchor) != Magic || readAt<uint16_t>(Anchor + 4) != 1 ||
      readAt<uint16_t>(Anchor + 6) != Count ||
      (Count < 32 && (Mask >> Count) != 0))
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  for (uint32_t K = 0; K != Count; ++K) {
    const unsigned char *Slot = Base + slotOffset(K);
    uint32_t Tag = readAt<uint32_t>(Slot);
    uint32_t RVA = readAt<uint32_t>(Slot + 4);
    const auto *Entry = readAt<const unsigned char *>(Anchor + 16 + 8 * K);
    bool Replaced = Tag == (TagBase ^ K) && RVA != 0;
    if (Replaced ? Entry != Base + RVA
                 : Tag != 0 || RVA != 0 || Entry || (K < 32 && (Mask >> K) & 1))
      __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  }
}

void NTAPI tlsCallback(PVOID, DWORD Reason, PVOID) {
  if (Reason == DLL_PROCESS_ATTACH)
    checkOverrideTable();
}

} // namespace

// After the null that opens .CRT$XLA, and before the callbacks of the program,
// which take .CRT$XLB onwards.
#pragma section(".CRT$XLAA", read)
extern "C" __declspec(allocate(".CRT$XLAA"))
const PIMAGE_TLS_CALLBACK __ovr_check = tlsCallback;

WINCRT_INCLUDE(_tls_used)
