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
// 0x80, the anchor's RVA u32 at 0x88, the block's RVA u32 at 0x8C and a
// reserved u64 at 0x90. The block of the defaults replaced by name holds the
// magic u32, the version and the number of entries u16, the overflow list's
// RVA and length u32 and two reserved u64 at 0x7E0, then 64 buckets of
// {key u64, rva u32, info u32} from 0x800; the overflow list has their layout.
// The anchor holds the magic, version and slot count, the mask of the slots
// that name a replacement rather than a forwarded one, the number of the
// block's entries, then for each slot the address its table entry names, or
// null, then for each entry of the block, in the block's order, its key and
// the address it names. This only reads.
void checkOverrideTable() {
  constexpr uint32_t Magic = 0x7B3AC2D5;
  constexpr uint32_t TagBase = 0xC5E1A7F3;
  constexpr uint32_t BlockMagic = 0x2F1B8E57;
  const auto *Base = reinterpret_cast<const unsigned char *>(&__ImageBase);
  const unsigned char *Anchor = __ovr_anchor;
  uint16_t Count = readAt<uint16_t>(Base + 0x86);
  uint32_t NewHeader = readAt<uint32_t>(Base + 0x3C);
  if (readAt<uint32_t>(Base + 0x80) != Magic ||
      readAt<uint16_t>(Base + 0x84) != 1 || Count < 8 ||
      readAt<uint64_t>(Base + 0x90) != 0 ||
      Base + readAt<uint32_t>(Base + 0x88) != Anchor ||
      NewHeader < slotOffset(Count - 1) + 8)
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

  uint32_t Named = readAt<uint32_t>(Anchor + 12);
  uint32_t Block = readAt<uint32_t>(Base + 0x8C);
  if (!Block) {
    if (Named)
      __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    return;
  }
  const unsigned char *Overflow = Base + readAt<uint32_t>(Base + 0x7E8);
  uint32_t OverflowCount = readAt<uint32_t>(Base + 0x7EC);
  if (Block != 0x7E0 || NewHeader > 0x7E0 ||
      readAt<uint32_t>(Base + NewHeader + 0x54) < 0xC00 ||
      readAt<uint32_t>(Base + 0x7E0) != BlockMagic ||
      readAt<uint16_t>(Base + 0x7E4) != 1 ||
      readAt<uint16_t>(Base + 0x7E6) != Named ||
      readAt<uint64_t>(Base + 0x7F0) != 0 ||
      readAt<uint64_t>(Base + 0x7F8) != 0 ||
      (OverflowCount && Overflow == Base))
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  // Each entry of the block, in the block's order, against the anchor's.
  const unsigned char *Expected = Anchor + 16 + 8 * Count;
  uint32_t Seen = 0;
  auto Check = [&](const unsigned char *E) {
    uint32_t RVA = readAt<uint32_t>(E + 8);
    if (Seen == Named || !RVA ||
        readAt<uint64_t>(E) != readAt<uint64_t>(Expected) ||
        readAt<const unsigned char *>(Expected + 8) != Base + RVA)
      __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    Expected += 16;
    ++Seen;
  };
  for (uint32_t B = 0; B != 64; ++B) {
    const unsigned char *E = Base + 0x800 + 16 * B;
    if (readAt<uint64_t>(E))
      Check(E);
    else if (readAt<uint64_t>(E + 8))
      __fastfail(FAST_FAIL_FATAL_APP_EXIT);
  }
  for (uint32_t I = 0; I != OverflowCount; ++I)
    Check(Overflow + 16 * I);
  if (Seen != Named)
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
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
