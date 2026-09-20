//===-- weak_interposition.cpp - Program-wide weak definitions ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A weak function definition means that it is the implementation's fallback
// and that a strong definition elsewhere in the program supersedes it. PE
// stops that at the image boundary, so a program replacing operator new cannot
// reach the copy the C++ runtime bound, and blocks allocated on one side and
// released on the other meet two allocators.
//
// The compiler gives each weak definition an entry that forwards through a
// pointer, and a record in .wkintp naming it by a hash of its name. A program
// makes a replacement reachable by exporting it, which is what the dynamic
// symbol table does on ELF, and the linker writes the hashed names of an
// executable's exports into .wkpub. This walks the records and binds the ones
// the program names.
//
// Only the program interposes. Nothing here writes to another image, reads
// another image's private data, or walks the loader's module list.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <string.h>

namespace {

// One weak definition, as the compiler emits it: the 128-bit xxh3 of the
// symbol name, the pointer the definition's entry reads, and the definition
// itself.
struct Record {
  uint64_t NameLow;
  uint64_t NameHigh;
  void *Target;
  void *Own;
};

// The head of the table the linker writes into an executable: the hashed names
// of the image's exports, sorted on the low half, followed by their RVAs in a
// parallel array that a search does not touch.
struct Published {
  uint32_t Count;
  uint32_t Reserved;
};

constexpr uintptr_t PageMask = ~uintptr_t{0xFFF};

// Section names are eight bytes, padded with zeros.
constexpr char PublishedSection[] = ".wkpub\0";

} // namespace

extern "C" {
extern const IMAGE_DOS_HEADER __ImageBase;
// Defined by the linker over the records; equal when there are none. Neither
// is an object of a known size, which is what lets the run be walked and one
// of the pointers in it be written. Naming them is also what tells the linker
// that this image takes part.
extern Record __wkintp_start[], __wkintp_end[];
}

namespace wincrt {

namespace {

// The contents of a section of the given image, or null when it has none.
const void *findSection(const void *Base, const char *Name, uint32_t *Size) {
  auto *Dos = static_cast<const IMAGE_DOS_HEADER *>(Base);
  if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;
  auto *Headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(
      static_cast<const char *>(Base) + Dos->e_lfanew);
  if (Headers->Signature != IMAGE_NT_SIGNATURE)
    return nullptr;

  const IMAGE_SECTION_HEADER *Section = IMAGE_FIRST_SECTION(Headers);
  for (WORD I = 0; I != Headers->FileHeader.NumberOfSections; ++I, ++Section)
    if (memcmp(Section->Name, Name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
      *Size = Section->Misc.VirtualSize;
      return static_cast<const char *>(Base) + Section->VirtualAddress;
    }
  return nullptr;
}

// The program's table, once its bounds hold. An image built by other tools has
// none, and one whose table does not describe itself is left alone rather than
// read: the only thing at stake is whether a weak definition is superseded,
// and the image's own is always a correct answer.
const Published *programTable(const void *Program, uint32_t *Count) {
  uint32_t Size = 0;
  auto *Table =
      static_cast<const Published *>(findSection(Program, PublishedSection,
                                                 &Size));
  if (!Table || Size < sizeof(Published))
    return nullptr;
  if (Table->Count > (Size - sizeof(Published)) / 20)
    return nullptr;
  *Count = Table->Count;
  return Table;
}

const uint64_t *hashesOf(const Published *Table) {
  return reinterpret_cast<const uint64_t *>(Table + 1);
}

const uint32_t *rvasOf(const Published *Table, uint32_t Count) {
  return reinterpret_cast<const uint32_t *>(hashesOf(Table) + 2 * Count);
}

// What this record's definition must forward to, or null when the program does
// not supersede it. Sorted on the low half of the hash, as the linker wrote
// it, so the search compares the halves in the order the record holds them.
void *resolve(const Record &Entry, const void *Program, const Published *Table,
              uint32_t Count) {
  const uint64_t *Hashes = hashesOf(Table);
  for (uint32_t Low = 0, High = Count; Low < High;) {
    uint32_t Middle = Low + (High - Low) / 2;
    uint64_t MiddleLow = Hashes[2 * Middle];
    uint64_t MiddleHigh = Hashes[2 * Middle + 1];
    if (MiddleLow == Entry.NameLow && MiddleHigh == Entry.NameHigh) {
      void *Target = const_cast<char *>(static_cast<const char *>(Program)) +
                     rvasOf(Table, Count)[Middle];
      // A definition that forwarded to itself would not return.
      return Target == Entry.Own ? nullptr : Target;
    }
    if (MiddleLow < Entry.NameLow ||
        (MiddleLow == Entry.NameLow && MiddleHigh < Entry.NameHigh))
      Low = Middle + 1;
    else
      High = Middle;
  }
  return nullptr;
}

// Restores a page the loop opened. Read-only rather than what the page held
// before, so that a page already writable when this ran does not stay so.
void sealPage(char *Page) {
  if (Page) {
    DWORD Previous;
    VirtualProtect(Page, 1, PAGE_READONLY, &Previous);
  }
}

} // namespace

void bindWeakDefinitions() {
  // Called from the TLS callback and from image initialization; the loader
  // lock (a DLL) or the single startup thread (an executable) orders the two.
  static bool Bound;
  if (Bound)
    return;
  Bound = true;

  if (__wkintp_start == __wkintp_end)
    return;

  // Nothing loaded later supersedes the program's own definitions, so an
  // executable binds nothing and never opens a page.
  const void *Program = GetModuleHandleW(nullptr);
  if (!Program || Program == &__ImageBase)
    return;

  uint32_t Count = 0;
  const Published *Table = programTable(Program, &Count);
  if (!Table || Count == 0)
    return;

  char *Page = nullptr;
  for (Record *Entry = __wkintp_start; Entry != __wkintp_end; ++Entry) {
    void *Target = resolve(*Entry, Program, Table, Count);
    if (!Target)
      continue;
    char *SlotPage = reinterpret_cast<char *>(
        reinterpret_cast<uintptr_t>(&Entry->Target) & PageMask);
    if (SlotPage != Page) {
      sealPage(Page);
      Page = nullptr;
      DWORD Previous;
      if (!VirtualProtect(SlotPage, 1, PAGE_READWRITE, &Previous))
        continue;
      Page = SlotPage;
    }
    Entry->Target = Target;
  }
  sealPage(Page);

  // The window above is the one the loader itself opens to write an import
  // address or a guard function pointer, and the loader reads those back once
  // it has closed it. Do the same. A pointer that does not hold what was
  // stored means either that the page was written by something else or that
  // it could not be opened, and an image whose weak definition was meant to
  // be superseded and is not will allocate on one side of a boundary and
  // release on the other.
  for (Record *Entry = __wkintp_start; Entry != __wkintp_end; ++Entry)
    if (Entry->Target != resolve(*Entry, Program, Table, Count))
      __fastfail(FAST_FAIL_MRDATA_MODIFIED);
}

} // namespace wincrt
