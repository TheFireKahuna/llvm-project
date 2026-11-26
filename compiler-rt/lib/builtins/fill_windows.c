//===-- fill_windows.c - Binding that an image does for itself ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The address of a delay-loaded function is the exporting DLL's own wherever
// a program can observe it, while the DLL stays delayed. The linker gives
// each such DLL an ensure stub, which code runs before it observes one of the
// DLL's addresses while the DLL's gate still holds its thunk, and a list of
// what holds the addresses. The stub calls __llvm_delay_resolve with the list.
//
// The loader is the only writer of a read-only word: it binds every word of
// a delay descriptor's table under its own bracket and reports nothing for a
// DLL or an export that is missing, whose words keep their thunks. This file
// only writes the words of writable data, which no table covers.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>

#if defined(__x86_64__)
#define NTAPI __attribute__((ms_abi))
#else
#define NTAPI
#endif

extern char __ImageBase[];

// Exported by ntdll; no SDK header declares it.
__declspec(dllimport) void *NTAPI LdrResolveDelayLoadedAPI(
    void *ParentModuleBase, const void *DelayloadDescriptor,
    void *FailureDllHook, void *FailureSystemHook, void *ThunkAddress,
    uint32_t Flags);

// The list, as the linker writes it, with every address relative to the
// image base: Tables descriptors to resolve in order, the DLL's own and then
// its runs, each with the first word of its table and the number of its
// imports; Words words of writable data, each with the function's entry of
// the DLL's table and the thunk the word holds; and the gate's descriptor.
struct DelayTable {
  uint32_t Descriptor, Slot, Count;
};
struct DelayWord {
  uint32_t Word, Cell, Thunk;
};
struct DelayList {
  uint32_t Tables, Words;
};

// The address of a word, which the linker lists only where it is aligned.
static uintptr_t *wordAt(uint32_t Rva) {
  return (uintptr_t *)((uintptr_t)__ImageBase + Rva);
}

// The loader takes a word for unbound exactly when it points into the image.
static int isUnbound(const uintptr_t *Slot, uint32_t ImageSize) {
  return __atomic_load_n(Slot, __ATOMIC_RELAXED) - (uintptr_t)__ImageBase <
         ImageSize;
}

// Has the loader bind a descriptor's table. The loader does nothing for a
// word that is bound, so it is given the first that is not; and where it
// cannot allocate, it binds at most 128 words from the one it is given, so
// it is asked once for each 128 that hold an unbound word.
static void resolveTable(const struct DelayTable *Table, uint32_t ImageSize,
                         uint32_t Flags) {
  uintptr_t *Slot = wordAt(Table->Slot);
  for (uint32_t I = 0; I < Table->Count; ++I)
    if (isUnbound(&Slot[I], ImageSize)) {
      LdrResolveDelayLoadedAPI(__ImageBase, __ImageBase + Table->Descriptor, 0,
                               0, &Slot[I], Flags);
      I |= 127;
    }
}

// Binds what List names: the tables; then each writable word that still
// holds its thunk, to the address that the loader bound the function's entry
// to, so that a value the program stored in the word stays; and last the
// gate, so that a thread that finds the gate bound finds every word written.
// A word whose function the DLL does not export keeps its thunk, and where
// the DLL does not load so does the gate, and a later call binds them.
void __llvm_delay_resolve(const struct DelayList *List) {
  // The image's headers: the size of the image, and from the load
  // configuration the search path of /dependentloadflag, of which the loader
  // admits these flags for a delayed load, so that this load finds the DLL
  // that a call of one of its functions does.
  const char *Optional =
      __ImageBase + *(const int32_t *)(__ImageBase + 0x3c) + 24;
  uint32_t ImageSize = *(const uint32_t *)(Optional + 56);
  uint32_t Config = *(const uint32_t *)(Optional + 112 + 10 * 8);
  uint32_t Flags = 0;
  if (Config && *(const uint32_t *)(__ImageBase + Config) >= 80)
    Flags = *(const uint16_t *)(__ImageBase + Config + 78) & 0x7b00;

  const struct DelayTable *Table = (const struct DelayTable *)(List + 1);
  const struct DelayWord *Word =
      (const struct DelayWord *)(Table + List->Tables);
  const struct DelayTable *Gate =
      (const struct DelayTable *)(Word + List->Words);
  for (; (const void *)Table != (const void *)Word; ++Table)
    resolveTable(Table, ImageSize, Flags);
  for (; (const void *)Word != (const void *)Gate; ++Word) {
    const uintptr_t *Cell = wordAt(Word->Cell);
    uintptr_t Thunk = (uintptr_t)__ImageBase + Word->Thunk;
    if (!isUnbound(Cell, ImageSize))
      __atomic_compare_exchange_n(wordAt(Word->Word), &Thunk,
                                  __atomic_load_n(Cell, __ATOMIC_RELAXED), 0,
                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  }
  resolveTable(Gate, ImageSize, Flags);
}
