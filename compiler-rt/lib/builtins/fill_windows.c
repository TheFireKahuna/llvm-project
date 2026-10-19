//===-- fill_windows.c - The start-up fill of an import-slots image -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The words of an image linked with -import-slots that the loader cannot
// write, written once at start-up from lists the linker builds. The linker
// keeps each function only in an image that needs it: __llvm_fill first in the
// C initializer table, and __llvm_thread_local_slot_copy as an executable's
// first TLS callback. Both run before the security cookie is set, so they have
// no stack protector, and call nothing that has a type check's prefix.
//
//===----------------------------------------------------------------------===//

#include <stddef.h>
#include <stdint.h>

#pragma clang attribute push(                                                  \
    __attribute__((no_stack_protector, no_sanitize("kcfi"), minsize)),         \
    apply_to = function)

typedef uint64_t __attribute__((aligned(4))) uint64_a4;

// The fields of the TEB, PEB, PEB_LDR_DATA and LDR_DATA_TABLE_ENTRY read here.
enum {
  tebThreadLocalStoragePointer = 0x58,
  tebPeb = 0x60,
  pebLdr = 0x18,
  pebImageBase = 0x10,
  pebCrossProcessFlags = 0x50,
  ldrInLoadOrderModuleList = 0x10,
  entryDllBase = 0x30,
  entrySizeOfImage = 0x40,
  entryFlags = 0x68,
  entryParentDllBase = 0xB8,
  entryLoadReason = 0x10C,
  processStaticImport = 0x20,
  loadReasonDelayloadDependency = 3,
  pageReadOnly = 2,
  pageReadWrite = 4,
};

// The words of the header page that the linker writes: the executable's
// override table and its anchor, its copy's tag, and the offer tables' tags.
enum {
  overrideTable = 0x80,
  overrideCount = 0x86,
  overrideAnchor = 0x88,
  copyTagOffset = 0x1C,
  dllOfferTable = 0x30,
  executableOfferTable = 0x38,
};
static const uint32_t overrideTableMagic = 0x7B3AC2D5;
static const uint32_t copyTag = 0x9E3779B9;
static const uint32_t dllOfferTableTag = 0x6C8E9CF5;
static const uint32_t offerTableMagic = 0x4F7A1D2B;

// A cell's kind: bound by name or by the override table's slot, either one
// possibly in the thread-local template; or, for a vague entity, whose entry
// names a word holding the instance, the RVA of the name a search of the
// loaded modules binds it by, or cellNoName.
enum {
  cellByName = 0,
  cellNoName = 1,
  cellSlot = 2,
  cellThreadLocal = 4,
  cellName = 0x1000,
};

enum { fillCallsCopy = 1, fillWordsInPlace = 2 };

struct range {
  uint32_t rva, size;
};

// The cells of one key follow each other; bit 0 of the key marks a cell in
// read-only data. A slot's key holds the offset of the slot's RVA in its top
// half and the slot shifted left by one in its bottom.
struct cell {
  uint64_t key;
  uint32_t rva, kind;
};

struct residualWord {
  uint32_t word, import;
  int64_t addend;
};

// name is the RVA of the import's name, and dll that of the UTF-16 name of the
// DLL a weak import is resolved in, or, for an import from the executable, 1 if it
// is weak and 0 if not.
struct runTimeCell {
  uint32_t cell, name, dll;
};

// The fill's list: the number of each kind of entry, the section it seals,
// this DLL's key, by which an image that delay-loads it lists it, and how many
// of the residual words, the last, are in the thread-local template. The
// ranges, the cells, the residual words and the run-time cells follow, in
// that order.
struct fillList {
  uint32_t ranges, cells, words, runTimeCells;
  uint32_t sealed, sealedSize;
  uint64_t selfKey;
  uint32_t flags, threadLocalWords;
};

struct copyList {
  uint64_t slots;
  struct {
    uint32_t rva, reserved;
    uint64_t lookup;
  } slot[];
};

// An offer table: its header, then a filter word for each four entities, then
// the buckets, then the keys of the DLLs the image delay-loads.
struct offerTable {
  uint32_t magic, flags, buckets, reserved, filterMask, delayLoads,
      delayLoadsOffset, bucketsOffset;
  uint64_t filter[];
};

struct bucket {
  uint32_t key, rva, name;
};

typedef struct {
  uint16_t length, maximumLength;
  const char *buffer;
} String;

__attribute__((dllimport)) int32_t NtProtectVirtualMemory(void *, void **,
                                                          size_t *, uint32_t,
                                                          uint32_t *);
__attribute__((dllimport)) int32_t LdrGetProcedureAddressForCaller(
    void *, const String *, uint32_t, void **, uint32_t, void *);
__attribute__((dllimport)) int32_t LdrGetDllHandleEx(uint32_t, void *, void *,
                                                     const String *, void **);
__attribute__((dllimport)) int32_t LdrAddRefDll(uint32_t, void *);

#pragma GCC visibility push(hidden)
extern char __ImageBase[];
extern const struct fillList __llvm_fill_list;
extern const struct copyList __llvm_thread_local_slot_copy_list;
int __llvm_fill(void);

static char *getTEB(void) {
  char *teb;
#if defined(__x86_64__)
  __asm__("movq %%gs:0x30, %0" : "=r"(teb));
#else
  __asm__("mov %0, x18" : "=r"(teb));
#endif
  return teb;
}

static uint32_t read32(const char *p) { return *(const uint32_t *)p; }
static char *readPointer(const char *p) { return *(char *const *)p; }

// The current thread's block of image's thread-local data, offset so that an
// RVA in the template indexes it: the TLS directory gives the template's
// start and the index, as addresses.
static char *threadBlock(char *image) {
  const char *nt = image + read32(image + 0x3C);
  const char *dir = image + read32(nt + 24 + 112 + 9 * 8);
  char **blocks = (char **)readPointer(getTEB() + tebThreadLocalStoragePointer);
  return blocks[read32(readPointer(dir + 16))] - (readPointer(dir) - image);
}

static uint16_t length(const char *s) {
  uint16_t n = 0;
  while (s[n])
    ++n;
  return n;
}

// Writes each of image's thread-local import slots into the main thread's
// block, which the loader filled from the template before it bound them, if
// the block's word still holds the slot's lookup entry. The process is still
// initializing, so no other thread runs.
static void copySlots(char *image, const struct copyList *l) {
  char *block = threadBlock(image);
  for (uint32_t i = 0; i != l->slots; ++i) {
    uint64_t *word = (uint64_t *)(block + l->slot[i].rva);
    if (*word == l->slot[i].lookup)
      *word = *(uint64_t *)(image + l->slot[i].rva);
  }
}

static int32_t protect(uint32_t rva, size_t size, uint32_t protection) {
  void *base = __ImageBase + rva;
  uint32_t old;
  return NtProtectVirtualMemory((void *)-1, &base, &size, protection, &old);
}

// Makes each range of the read-only cells writable, failing at the first
// call that fails, or read-only again, trying every range and failing if any
// stays writable.
static int32_t protectRanges(const struct fillList *l, uint32_t protection) {
  const struct range *r = (const struct range *)(l + 1);
  int32_t result = 0;
  for (uint32_t i = 0; i != l->ranges; ++i) {
    int32_t status = protect(r[i].rva, r[i].size, protection);
    if (status && protection == pageReadWrite)
      return status;
    if (!result)
      result = status;
  }
  return result;
}

// Whether module m is in the executable's scope: the executable, one loaded
// while the process initialized, or one delay-loaded by a module of that
// scope, as a dependency joins its parent's scope on ELF.
static int inScope(char *head, char *exe, char *m) {
  for (int bound = 64;;) {
    if (m == exe || read32(m + entryFlags) & processStaticImport)
      return 1;
    if (read32(m + entryLoadReason) != loadReasonDelayloadDependency ||
        --bound == 0)
      return 0;
    char *parent = readPointer(m + entryParentDllBase);
    for (m = readPointer(head);
         m != head && readPointer(m + entryDllBase) != parent;
         m = readPointer(m))
      ;
    if (m == head)
      return 0;
  }
}

static int delayLoadsSelf(const struct offerTable *t, uint64_t selfKey) {
  const uint64_a4 *key =
      (const uint64_a4 *)((const char *)t + t->delayLoadsOffset);
  for (uint32_t i = 0; i != t->delayLoads; ++i)
    if (key[i] == selfKey)
      return 1;
  return 0;
}

// The offer table of module m, whose image is size bytes, if it fits there.
static const struct offerTable *getOfferTable(char *exe, char *m, char *base,
                                              uint32_t size) {
  uint32_t rva;
  if (m == exe)
    rva = read32(base + executableOfferTable);
  else if (read32(base + dllOfferTable) == dllOfferTableTag)
    rva = read32(base + dllOfferTable + 4);
  else
    return NULL;
  if (rva >= size || size - rva < sizeof(struct offerTable))
    return NULL;
  const struct offerTable *t = (const struct offerTable *)(base + rva);
  uint64_t left = size - rva;
  if (t->magic != offerTableMagic || !t->buckets ||
      (uint64_t)t->filterMask * 8 + sizeof(*t) + 8 > t->bucketsOffset ||
      (uint64_t)t->buckets * sizeof(struct bucket) + t->bucketsOffset > left ||
      (uint64_t)t->delayLoads * 8 + t->delayLoadsOffset > left)
    return NULL;
  return t;
}

// Whether the string at s, which ends within limit bytes, is name.
static int sameName(const char *s, const char *name, uint32_t limit) {
  for (; limit; --limit, ++s, ++name)
    if (*s != *name || !*s)
      return *s == *name;
  return 0;
}

// A vague entity's instance in a DLL loaded after the process initialized,
// from the offer tables of the modules of its scope, in load order: the
// executable's, those of the modules loaded with the process or delay-loaded
// into that scope, and those of the modules that delay-load this DLL. Pins
// the module whose instance it takes. Sets *status if a pin fails.
static void *search(const struct fillList *l, char *peb, uint64_t key,
                    const char *name, int32_t *status) {
  char *head = readPointer(peb + pebLdr) + ldrInLoadOrderModuleList;
  char *exe = readPointer(head);
  for (char *m = exe; m != head; m = readPointer(m)) {
    char *base = readPointer(m + entryDllBase);
    uint32_t size = read32(m + entrySizeOfImage);
    const struct offerTable *t;
    if (base == __ImageBase || !(t = getOfferTable(exe, m, base, size)) ||
        (!inScope(head, exe, m) && !delayLoadsSelf(t, l->selfKey)))
      continue;
    uint64_t filter = t->filter[key >> 13 & t->filterMask];
    if (!(filter >> (key >> 1 & 63) & 1) || !(filter >> (key >> 7 & 63) & 1))
      continue;
    const struct bucket *b =
        (const struct bucket *)((const char *)t + t->bucketsOffset);
    uint32_t top = key >> 32, n = t->buckets;
    for (uint32_t i = (uint64_t)top * n >> 32, left = n; left && b[i].rva;
         --left, i = i + 1 == n ? 0 : i + 1) {
      if (b[i].key != top || b[i].name >= size)
        continue;
      if (!sameName(base + b[i].name, name, size - b[i].name) ||
          (uint64_t)b[i].rva + 8 > size)
        continue;
      char *instance = base + b[i].rva;
      if (!(t->flags & 1))
        instance = readPointer(instance);
      if (instance && m != exe)
        *status = LdrAddRefDll(1, base);
      return *status < 0 ? NULL : instance;
    }
  }
  return NULL;
}

// The address the executable's table gives a cell's key, if it gives one: for
// a slot, the replacement the anchor's mask names; for a name, the address the
// anchor's entry names, or for a vague entity the one its word holds.
static int lookUp(char *exe, const struct cell *c, uint64_t key, char **value) {
  char *anchor = exe + read32(exe + overrideAnchor);
  if ((c->kind & ~cellThreadLocal) == cellSlot) {
    if (!(read32(anchor + 8) >> ((uint32_t)key >> 1 & 31) & 1))
      return 0;
    *value = exe + read32(exe + (key >> 32));
    return 1;
  }
  const uint64_t *entry =
      (const uint64_t *)(anchor + 16 +
                         8 * *(const uint16_t *)(exe + overrideCount));
  for (uint32_t n = read32(anchor + 12); n; --n, entry += 2) {
    if (entry[0] != key)
      continue;
    *value = (char *)entry[1];
    if (c->kind == cellNoName || c->kind >= cellName)
      *value = readPointer(*value);
    return 1;
  }
  return 0;
}

int __llvm_fill(void) {
  const struct fillList *l = &__llvm_fill_list;
  char *peb = readPointer(getTEB() + tebPeb);
  int initializing = read32(peb + pebCrossProcessFlags) & 2;
  char *exe = readPointer(peb + pebImageBase);
  int32_t status = 0;
  int writable = 0;

  // Code of the executable that runs while the DLLs initialize, through this
  // DLL's imports from it or its cells, sees its thread-local slots bound in
  // the main thread.
  if (l->flags & fillCallsCopy && initializing &&
      read32(exe + copyTagOffset) == copyTag)
    copySlots(exe,
              (const struct copyList *)(exe + read32(exe + copyTagOffset + 4)));

  // The cells, each key's written together where the executable's table or a
  // search gives it an address.
  if (read32(exe + overrideTable) != overrideTableMagic)
    exe = NULL;
  const struct cell *c = (const struct cell *)((const struct range *)(l + 1) +
                                               l->ranges),
                    *cellsEnd = c + l->cells;
  while (c != cellsEnd) {
    uint64_t key = c->key & ~(uint64_t)1;
    char *value = NULL;
    int found = exe && lookUp(exe, c, key, &value);
    if (!found && !initializing && c->kind >= cellName) {
      value = search(l, peb, key, __ImageBase + c->kind, &status);
      if (status < 0)
        goto done;
      found = value != NULL;
    }
    for (; c != cellsEnd && (c->key & ~(uint64_t)1) == key; ++c) {
      if (!found)
        continue;
      if (c->key & 1 && !writable) {
        writable = 1;
        if ((status = protectRanges(l, pageReadWrite)))
          goto done;
      }
      *(char **)(__ImageBase + c->rva) = value;
      if (c->kind < cellName && c->kind & cellThreadLocal)
        *(char **)(threadBlock(__ImageBase) + c->rva) = value;
    }
  }

  // The imports bound at run time: one from the executable in the process's
  // own, failing the fill where it is missing and not weak, and a weak one
  // from a DLL in that DLL if it is loaded. Flag 1 binds without initializing
  // the module, as the loader's snap does.
  const struct residualWord *w = (const struct residualWord *)cellsEnd;
  const struct runTimeCell *r = (const struct runTimeCell *)(w + l->words);
  for (uint32_t i = 0; i != l->runTimeCells; ++i, ++r) {
    void *module = readPointer(peb + pebImageBase);
    if (r->dll > 1) {
      const uint16_t *wide = (const uint16_t *)(__ImageBase + r->dll);
      uint16_t n = 0;
      while (wide[n])
        ++n;
      String dll = {(uint16_t)(2 * n), (uint16_t)(2 * n), (const char *)wide};
      if (LdrGetDllHandleEx(1, NULL, NULL, &dll, &module) < 0)
        continue;
    }
    String name = {length(__ImageBase + r->name),
                   (uint16_t)(length(__ImageBase + r->name) + 1),
                   __ImageBase + r->name};
    int32_t lookup = LdrGetProcedureAddressForCaller(
        module, &name, 0, (void **)(__ImageBase + r->cell), 1, __llvm_fill);
    if (lookup < 0 && !r->dll) {
      status = lookup;
      goto done;
    }
  }

  // A residual word in read-only data outside the sealed section is written
  // where it is, and one in the thread-local template in the loading thread's
  // block too, which the loader copied from the template before.
  if (l->flags & fillWordsInPlace && !writable) {
    writable = 1;
    if ((status = protectRanges(l, pageReadWrite)))
      goto done;
  }
  for (uint32_t i = 0; i != l->words; ++i, ++w) {
    uint64_t value = *(uint64_t *)(__ImageBase + w->import) + w->addend;
    *(uint64_t *)(__ImageBase + w->word) = value;
    if (i >= l->words - l->threadLocalWords)
      *(uint64_t *)(threadBlock(__ImageBase) + w->word) = value;
  }

done:
  // A cell left writable fails the image's initialization, as one left
  // unwritten does.
  if (writable) {
    int32_t reseal = protectRanges(l, pageReadOnly);
    if (!status)
      status = reseal;
  }
  if (!status && l->sealedSize)
    status = protect(l->sealed, l->sealedSize, pageReadOnly);
  return status;
}

// The executable's first TLS callback: the copy of its thread-local import
// slots, which acts only while the process initializes. The fill of a DLL
// that may reach the executable while the DLLs initialize runs it first, from
// the list the executable's header names, so it may run any number of times;
// it never undoes an assignment.
void __llvm_thread_local_slot_copy(void) {
  if (read32(readPointer(getTEB() + tebPeb) + pebCrossProcessFlags) & 2)
    copySlots(__ImageBase, &__llvm_thread_local_slot_copy_list);
}

#pragma clang attribute pop
#pragma GCC visibility pop
