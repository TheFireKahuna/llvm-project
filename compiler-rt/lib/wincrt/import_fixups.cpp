//===-- import_fixups.cpp - Addends of in-place import slots --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Static data that holds the address of a symbol from another DLL is filled
// by the loader, which writes the plain address. For a pointer into the
// middle of an imported object the linker records the slot and its addend,
// and this code adds the addend before any initializer of the image runs. The
// records are sorted by address; one flagged read-only lies in a page the
// loader has restored to read-only.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

namespace {

struct ImportFixup {
  DWORD Rva;
  DWORD Flags; // 1: the slot is in read-only memory.
  LONGLONG Addend;
};

constexpr uintptr_t PageMask = ~uintptr_t{0xFFF};

} // namespace

extern "C" {
// Not const: the slots written below are addressed from it, and a store
// through a pointer derived from a constant is dead to the optimizer.
extern IMAGE_DOS_HEADER __ImageBase;
// Defined by the linker over the records; equal when there are none.
extern const ImportFixup __import_fixups_start[], __import_fixups_end[];
}

namespace wincrt {

void applyImportFixups() {
  // Called from the TLS callback and from image initialization; the loader
  // lock (a DLL) or the single startup thread (an executable) orders the two.
  static bool Applied;
  if (Applied)
    return;
  Applied = true;

  char *Base = reinterpret_cast<char *>(&__ImageBase);
  char *Page = nullptr;
  DWORD Protection = 0;
  for (const ImportFixup *Fixup = __import_fixups_start;
       Fixup != __import_fixups_end; ++Fixup) {
    char *Slot = Base + Fixup->Rva;
    if (Fixup->Flags & 1) {
      char *SlotPage =
          reinterpret_cast<char *>(reinterpret_cast<uintptr_t>(Slot) & PageMask);
      if (SlotPage != Page) {
        if (Page)
          VirtualProtect(Page, 1, Protection, &Protection);
        Page = SlotPage;
        VirtualProtect(Page, 1, PAGE_READWRITE, &Protection);
      }
    }
    *reinterpret_cast<uintptr_t *>(Slot) += Fixup->Addend;
  }
  if (Page)
    VirtualProtect(Page, 1, Protection, &Protection);
}

namespace {

void NTAPI tlsCallback(void *, DWORD Reason, void *) {
  if (Reason == DLL_PROCESS_ATTACH)
    applyImportFixups();
}

} // namespace
} // namespace wincrt

// A TLS callback of an executable runs before its entry point, ahead of any
// user callback that could read a slot.
#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB"))
    PIMAGE_TLS_CALLBACK __wincrt_import_fixups_callback = wincrt::tlsCallback;
