//===- DLL.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines various types of chunks for the DLL import or export
// descriptor tables. They are inherently Windows-specific.
// You need to read Microsoft PE/COFF spec to understand details
// about the data structures.
//
// If you are not particularly interested in linking against Windows
// DLL, you can skip this file, and you should still be able to
// understand the rest of the linker.
//
//===----------------------------------------------------------------------===//

#include "DLL.h"
#include "COFFLinkerContext.h"
#include "Chunks.h"
#include "MarkLive.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Common/AArch64Instructions.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ARMWinEH.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Win64EH.h"
#include <array>

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::COFF;

namespace lld::coff {
namespace {

// Import table

// A chunk for the import descriptor table.
class HintNameChunk : public NonSectionChunk {
public:
  HintNameChunk(StringRef n, uint16_t h) : name(n), hint(h) {}

  size_t getSize() const override {
    // Starts with 2 byte Hint field, followed by a null-terminated string,
    // ends with 0 or 1 byte padding.
    return alignTo(name.size() + 3, 2);
  }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());
    write16le(buf, hint);
    memcpy(buf + 2, name.data(), name.size());
  }

private:
  StringRef name;
  uint16_t hint;
};

// A chunk for the import descriptor table.
class LookupChunk : public NonSectionChunk {
public:
  explicit LookupChunk(COFFLinkerContext &ctx, Chunk *c)
      : hintName(c), ctx(ctx) {
    setAlignment(ctx.config.wordsize);
  }
  size_t getSize() const override { return ctx.config.wordsize; }

  void writeTo(uint8_t *buf) const override {
    if (ctx.config.is64())
      write64le(buf, hintName->getRVA());
    else
      write32le(buf, hintName->getRVA());
  }

  Chunk *hintName;

private:
  COFFLinkerContext &ctx;
};

// A chunk for the import descriptor table.
// This chunk represent import-by-ordinal symbols.
// See Microsoft PE/COFF spec 7.1. Import Header for details.
class OrdinalOnlyChunk : public NonSectionChunk {
public:
  explicit OrdinalOnlyChunk(COFFLinkerContext &c, uint16_t v)
      : ordinal(v), ctx(c) {
    setAlignment(ctx.config.wordsize);
  }
  size_t getSize() const override { return ctx.config.wordsize; }

  void writeTo(uint8_t *buf) const override {
    // An import-by-ordinal slot has MSB 1 to indicate that
    // this is import-by-ordinal (and not import-by-name).
    if (ctx.config.is64()) {
      write64le(buf, (1ULL << 63) | ordinal);
    } else {
      write32le(buf, (1ULL << 31) | ordinal);
    }
  }

  uint16_t ordinal;

private:
  COFFLinkerContext &ctx;
};

// A chunk for the import descriptor table.
class ImportDirectoryChunk : public NonSectionChunk {
public:
  explicit ImportDirectoryChunk(Chunk *n) : dllName(n) { setAlignment(4); }
  size_t getSize() const override { return sizeof(ImportDirectoryTableEntry); }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());

    auto *e = (coff_import_directory_table_entry *)(buf);
    e->ImportLookupTableRVA = lookupTab->getRVA() + lookupOffset;
    e->NameRVA = dllName->getRVA();
    e->ImportAddressTableRVA = addressTab->getRVA() + addressOffset;
  }

  Chunk *dllName;
  Chunk *lookupTab;
  Chunk *addressTab;
  // Where the tables start in lookupTab and addressTab: a pooled lookup table
  // starts inside the pool, and a run of in-place slots inside the section
  // that holds it.
  uint32_t lookupOffset = 0;
  uint32_t addressOffset = 0;
};

// The lookup tables of an image that has in-place slots, whose runs each
// need one. A table is the keys of its imports, each the import's place in
// imports counted from one, then a zero. The builder lays equal tables out
// once and a table that ends another inside it, and the keys are written as
// the imports' lookup entries.
class LookupPoolChunk : public NonSectionChunk {
public:
  LookupPoolChunk() : builder(StringTableBuilder::RAW, Align(8)) {
    setAlignment(8);
  }
  size_t getSize() const override { return builder.getSize(); }

  void writeTo(uint8_t *buf) const override {
    builder.write(buf);
    for (size_t i = 0, e = getSize(); i != e; i += 8)
      if (uint64_t key = read64le(buf + i))
        write64le(buf + i, imports[key - 1]->getLookupEntry());
  }

  StringTableBuilder builder;
  std::vector<DefinedImportData *> imports;
};

// A chunk representing null terminator in the import table.
// Contents of this chunk is always null bytes.
class NullChunk : public NonSectionChunk {
public:
  explicit NullChunk(size_t n, uint32_t align) : size(n) {
    setAlignment(align);
  }
  explicit NullChunk(COFFLinkerContext &ctx)
      : NullChunk(ctx.config.wordsize, ctx.config.wordsize) {}
  explicit NullChunk(COFFLinkerContext &ctx, size_t n)
      : NullChunk(n, ctx.config.wordsize) {}
  size_t getSize() const override { return size; }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, size);
  }

private:
  size_t size;
};

// A chunk for ARM64EC auxiliary IAT.
class AuxImportChunk : public NonSectionChunk {
public:
  explicit AuxImportChunk(ImportFile *file) : file(file) {
    setAlignment(sizeof(uint64_t));
  }
  size_t getSize() const override { return sizeof(uint64_t); }

  void writeTo(uint8_t *buf) const override {
    uint64_t impchkVA = 0;
    if (file->impchkThunk)
      impchkVA =
          file->impchkThunk->getRVA() + file->symtab.ctx.config.imageBase;
    write64le(buf, impchkVA);
  }

  void getBaserels(std::vector<Baserel> *res) override {
    if (file->impchkThunk)
      res->emplace_back(rva, file->symtab.machine);
  }

private:
  ImportFile *file;
};

static std::vector<std::vector<DefinedImportData *>>
binImports(COFFLinkerContext &ctx,
           const std::vector<DefinedImportData *> &imports) {
  // Group DLL-imported symbols by DLL name because that's how
  // symbols are laid out in the import descriptor table.
  auto less = [&ctx](const std::string &a, const std::string &b) {
    return ctx.config.dllOrder[a] < ctx.config.dllOrder[b];
  };
  std::map<std::string, std::vector<DefinedImportData *>, decltype(less)> m(
      less);
  for (DefinedImportData *sym : imports)
    m[sym->getDLLName().lower()].push_back(sym);

  std::vector<std::vector<DefinedImportData *>> v;
  for (auto &kv : m) {
    // Sort symbols by name for each group.
    std::vector<DefinedImportData *> &syms = kv.second;
    llvm::sort(syms, [](DefinedImportData *a, DefinedImportData *b) {
      auto getBaseName = [](DefinedImportData *sym) {
        StringRef name = sym->getName();
        name.consume_front("__imp_");
        // Skip aux_ part of ARM64EC function symbol name.
        if (sym->file->impchkThunk)
          name.consume_front("aux_");
        return name;
      };
      return getBaseName(a) < getBaseName(b);
    });
    v.push_back(std::move(syms));
  }
  return v;
}

// See Microsoft PE/COFF spec 4.3 for details.

// A chunk for the delay import descriptor table etnry.
class DelayDirectoryChunk : public NonSectionChunk {
public:
  explicit DelayDirectoryChunk(Chunk *n) : dllName(n) { setAlignment(4); }

  size_t getSize() const override {
    return sizeof(delay_import_directory_table_entry);
  }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());

    auto *e = (delay_import_directory_table_entry *)(buf);
    e->Attributes = 1;
    e->Name = dllName->getRVA();
    e->ModuleHandle = moduleHandle->getRVA();
    e->DelayImportAddressTable = addressTab->getRVA() + addressOffset;
    e->DelayImportNameTable = nameTab->getRVA();
    if (unloadTab)
      e->UnloadDelayImportTable = unloadTab->getRVA();
    if (list)
      e->BoundDelayImportTable = list->getRVA();
  }

  Chunk *dllName;
  Chunk *moduleHandle;
  Chunk *addressTab;
  Chunk *nameTab;
  Chunk *unloadTab = nullptr;
  // Where the table starts in addressTab: a run starts inside the section
  // that holds it.
  uint32_t addressOffset = 0;
  // The list of the DLL whose gate the descriptor is for, which it names
  // where a bound table would be and with no time stamp, so that a helper
  // that resolves every descriptor of a DLL can tell the gate's and leave it
  // to the list's reader.
  Chunk *list = nullptr;
};

// What a DLL's ensure stub gives the runtime, compiler-rt's
// __llvm_delay_resolve: the DLL's delay descriptors in the order to resolve
// them, the DLL's own table and then its runs, each with the first word of
// its table and the number of its imports; the words of writable data to
// exchange once those are bound, each with the function's cell and the thunk
// the word holds; and the gate's descriptor, resolved last.
class DelayListChunk : public NonSectionChunk {
public:
  struct Table {
    Chunk *dir;
    Chunk *chunk;
    uint32_t offset;
    uint32_t count;
  };

  DelayListChunk() { setAlignment(4); }
  size_t getSize() const override {
    return 8 + 12 * (tables.size() + words.size() + 1);
  }

  void writeTo(uint8_t *buf) const override {
    write32le(buf, tables.size());
    write32le(buf + 4, words.size());
    buf += 8;
    auto writeTable = [&](const Table &t) {
      write32le(buf, t.dir->getRVA());
      write32le(buf + 4, t.chunk->getRVA() + t.offset);
      write32le(buf + 8, t.count);
      buf += 12;
    };
    for (const Table &t : tables)
      writeTable(t);
    for (const ImportSlot &w : words) {
      write32le(buf, w.chunk->getRVA() + w.offset);
      write32le(buf + 4, w.sym->getRVA());
      write32le(buf + 8,
                cast<Defined>(getScannedAt(*w.chunk, w.offset).sym)->getRVA());
      buf += 12;
    }
    writeTable(gate);
  }

  std::vector<Table> tables;
  std::vector<ImportSlot> words;
  Table gate;
};

// Initial contents for delay-loaded functions.
// This code calls __delayLoadHelper2 function to resolve a symbol
// which then overwrites its jump table slot with the result
// for subsequent function calls.
static const uint8_t thunkX64[] = {
    0x48, 0x8D, 0x05, 0, 0, 0, 0,       // lea     rax, [__imp_<FUNCNAME>]
    0xE9, 0, 0, 0, 0,                   // jmp     __tailMerge_<lib>
};

static const uint8_t tailMergeX64[] = {
    0x48, 0x89, 0x4C, 0x24, 0x08,          // mov    qword ptr [rsp+8], rcx
    0x48, 0x89, 0x54, 0x24, 0x10,          // mov    qword ptr [rsp+10h], rdx
    0x4C, 0x89, 0x44, 0x24, 0x18,          // mov    qword ptr [rsp+18h], r8
    0x4C, 0x89, 0x4C, 0x24, 0x20,          // mov    qword ptr [rsp+20h], r9
    0x48, 0x83, 0xEC, 0x68,                // sub    rsp, 68h
    0x66, 0x0F, 0x7F, 0x44, 0x24, 0x20,    // movdqa xmmword ptr [rsp+20h], xmm0
    0x66, 0x0F, 0x7F, 0x4C, 0x24, 0x30,    // movdqa xmmword ptr [rsp+30h], xmm1
    0x66, 0x0F, 0x7F, 0x54, 0x24, 0x40,    // movdqa xmmword ptr [rsp+40h], xmm2
    0x66, 0x0F, 0x7F, 0x5C, 0x24, 0x50,    // movdqa xmmword ptr [rsp+50h], xmm3
    0x48, 0x8B, 0xD0,                      // mov    rdx, rax
    0x48, 0x8D, 0x0D, 0, 0, 0, 0,          // lea    rcx, [___DELAY_IMPORT_...]
    0xE8, 0, 0, 0, 0,                      // call   __delayLoadHelper2
    0x66, 0x0F, 0x6F, 0x44, 0x24, 0x20,    // movdqa xmm0, xmmword ptr [rsp+20h]
    0x66, 0x0F, 0x6F, 0x4C, 0x24, 0x30,    // movdqa xmm1, xmmword ptr [rsp+30h]
    0x66, 0x0F, 0x6F, 0x54, 0x24, 0x40,    // movdqa xmm2, xmmword ptr [rsp+40h]
    0x66, 0x0F, 0x6F, 0x5C, 0x24, 0x50,    // movdqa xmm3, xmmword ptr [rsp+50h]
    0x48, 0x8B, 0x4C, 0x24, 0x70,          // mov    rcx, qword ptr [rsp+70h]
    0x48, 0x8B, 0x54, 0x24, 0x78,          // mov    rdx, qword ptr [rsp+78h]
    0x4C, 0x8B, 0x84, 0x24, 0x80, 0, 0, 0, // mov    r8, qword ptr [rsp+80h]
    0x4C, 0x8B, 0x8C, 0x24, 0x88, 0, 0, 0, // mov    r9, qword ptr [rsp+88h]
    0x48, 0x83, 0xC4, 0x68,                // add    rsp, 68h
    0xFF, 0xE0,                            // jmp    rax
};

static const uint8_t tailMergeUnwindInfoX64[] = {
    0x01,       // Version=1, Flags=UNW_FLAG_NHANDLER
    0x18,       // Size of prolog
    0x01,       // Count of unwind codes
    0x00,       // No frame register
    0x18, 0xC2, // Offset 0x18: UWOP_ALLOC_SMALL(0x68)
    0x00, 0x00  // Padding to align on 32-bits
};

// A DLL's ensure stub: it keeps every register, the flags and the extended
// state on a stack it aligns itself and touches page by page, and calls the
// runtime with the DLL's list in the first argument register of both calling
// conventions. The area is the size of KUSER_SHARED_DATA's XState.Size, the
// standard form of every enabled component, which bounds the compacted form
// of any. xsavec writes only the components in use, a component that XFD
// disables as not in use, and the xrstor of all of them initialises the
// rest, so that it restores the state whole and loads nothing that XFD
// disables.
static const uint8_t ensureX64[] = {
    0x55,                                     // push   rbp
    0x48, 0x89, 0xE5,                         // mov    rbp, rsp
    0x9C,                                     // pushfq
    0x50, 0x51, 0x52, 0x56, 0x57,             // push   rax, rcx, rdx, rsi, rdi
    0x41, 0x50, 0x41, 0x51,                   // push   r8, r9
    0x41, 0x52, 0x41, 0x53,                   // push   r10, r11
    0xFC,                                     // cld
    0x8B, 0x0C, 0x25, 0xE8, 0x03, 0xFE, 0x7F, // mov    ecx, [7FFE03E8h]
    0x48, 0x89, 0xE0,                         // mov    rax, rsp
    0x48, 0x29, 0xC8,                         // sub    rax, rcx
    0x48, 0x83, 0xE0, 0xC0,                   // and    rax, -64
    0x48, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00, // sub    rsp, 1000h
    0x48, 0x85, 0x24, 0x24,                   // test   qword ptr [rsp], rsp
    0x48, 0x39, 0xC4,                         // cmp    rsp, rax
    0x77, 0xF0,                               // ja     the sub
    0x48, 0x89, 0xC4,                         // mov    rsp, rax
    0x48, 0x8D, 0xBC, 0x24, 0x00, 0x02, 0x00, 0x00, // lea    rdi, [rsp+200h]
    0xB9, 0x08, 0x00, 0x00, 0x00,                   // mov    ecx, 8
    0x31, 0xC0,                                     // xor    eax, eax
    0xF3, 0x48, 0xAB,                               // rep stosq
    0xB8, 0xFF, 0xFF, 0xFF, 0xFF,                   // mov    eax, -1
    0x89, 0xC2,                                     // mov    edx, eax
    0x48, 0x0F, 0xC7, 0x24, 0x24,                   // xsavec64 [rsp]
    0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00,       // lea    rcx, [the list]
    0x48, 0x89, 0xCF,                               // mov    rdi, rcx
    0x48, 0x83, 0xEC, 0x20,                         // sub    rsp, 20h
    0xE8, 0x00, 0x00, 0x00, 0x00, // call   __llvm_delay_resolve
    0x48, 0x83, 0xC4, 0x20,       // add    rsp, 20h
    0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov    eax, -1
    0x89, 0xC2,                   // mov    edx, eax
    0x48, 0x0F, 0xAE, 0x2C, 0x24, // xrstor64 [rsp]
    0x48, 0x8D, 0x65, 0xB0,       // lea    rsp, [rbp-50h]
    0x41, 0x5B, 0x41, 0x5A,       // pop    r11, r10
    0x41, 0x59, 0x41, 0x58,       // pop    r9, r8
    0x5F, 0x5E, 0x5A, 0x59, 0x58, // pop    rdi, rsi, rdx, rcx, rax
    0x9D,                         // popfq
    0x5D,                         // pop    rbp
    0xC3,                         // ret
};

static const uint8_t ensureUnwindInfoX64[] = {
    0x01,       // Version=1, Flags=UNW_FLAG_NHANDLER
    0x04,       // Size of prolog
    0x02,       // Count of unwind codes
    0x05,       // Frame register rbp, offset 0
    0x04, 0x03, // Offset 4: UWOP_SET_FPREG
    0x01, 0x50, // Offset 1: UWOP_PUSH_NONVOL(rbp)
};

// A guard's leaf pushes one register, rcx or rax, and pops it at the
// epilogue before its calls of the ensure stubs; one that replays pushes rcx
// and rax, and its epilogue ends with a jump.
static const uint8_t guardUnwindInfoX64[3][8] = {
    {0x01, 0x01, 0x01, 0x00, 0x01, 0x00}, // Offset 1: UWOP_PUSH_NONVOL(rax)
    {0x01, 0x01, 0x01, 0x00, 0x01, 0x10}, // Offset 1: UWOP_PUSH_NONVOL(rcx)
    {0x01, 0x02, 0x02, 0x00,              // Prolog size 2, 2 codes
     0x02, 0x00,                          // Offset 2: UWOP_PUSH_NONVOL(rax)
     0x01, 0x10},                         // Offset 1: UWOP_PUSH_NONVOL(rcx)
};

// The same for ARM64: x0 to x17, the flags, the floating-point status and
// q0 to q31, in pairs that it loads in the reverse order of their stores.
static constexpr auto ensureARM64 = [] {
  std::array<uint8_t, 272> code{};
  size_t n = 0;
  auto put = [&](uint32_t insn) {
    for (int i = 0; i != 4; ++i)
      code[n++] = insn >> 8 * i;
  };
  // stp or ldp of xr and xr+1 at [sp, #8 * r], or of qr and qr+1 at
  // [sp, #160 + 16 * r].
  auto pair = [&](bool load, bool q, uint32_t r) {
    put((q ? 0xad000000 + (10 + r) * 0x8000 : 0xa9000000 + r * 0x8000) |
        load << 22 | (r + 1) << 10 | 31 << 5 | r);
  };
  put(0xa9bf7bfd); // stp     x29, x30, [sp, #-16]!
  put(0x910003fd); // mov     x29, sp
  put(0xd10a83ff); // sub     sp, sp, #672
  for (uint32_t r = 0; r != 18; r += 2)
    pair(false, false, r);
  put(0xd53b4210); // mrs     x16, nzcv
  put(0xd53b4431); // mrs     x17, fpsr
  put(0xa90947f0); // stp     x16, x17, [sp, #144]
  for (uint32_t r = 0; r != 32; r += 2)
    pair(false, true, r);
  put(0x90000000); // adrp    x0, the list
  put(0x91000000); // add     x0, x0, :lo12:the list
  put(0x90000010); // adrp    x16, __llvm_delay_resolve
  put(0x91000210); // add     x16, x16, :lo12:__llvm_delay_resolve
  put(0xd63f0200); // blr     x16
  for (uint32_t r = 32; r != 0; r -= 2)
    pair(true, true, r - 2);
  put(0xa94947f0); // ldp     x16, x17, [sp, #144]
  put(0xd51b4210); // msr     nzcv, x16
  put(0xd51b4431); // msr     fpsr, x17
  for (uint32_t r = 18; r != 0; r -= 2)
    pair(true, false, r - 2);
  put(0x910a83ff); // add     sp, sp, #672
  put(0x910003bf); // mov     sp, x29
  put(0xa8c17bfd); // ldp     x29, x30, [sp], #16
  put(0xd65f03c0); // ret
  return code;
}();

// 68 instructions, and one epilogue, at the end, that shares the two words
// of the prologue's codes: sub sp, #672; mov x29, sp; stp x29, x30,
// [sp, #-16]!; end.
static const uint8_t ensureUnwindInfoARM64[] = {
    0x44, 0x00, 0x20, 0x10, 0xc0, 0x2a, 0xe1, 0x81, 0xe4, 0xe3, 0xe3, 0xe3,
};

static const uint8_t thunkX86[] = {
    0xB8, 0, 0, 0, 0,  // mov   eax, offset ___imp__<FUNCNAME>
    0xE9, 0, 0, 0, 0,  // jmp   __tailMerge_<lib>
};

static const uint8_t tailMergeX86[] = {
    0x51,              // push  ecx
    0x52,              // push  edx
    0x50,              // push  eax
    0x68, 0, 0, 0, 0,  // push  offset ___DELAY_IMPORT_DESCRIPTOR_<DLLNAME>_dll
    0xE8, 0, 0, 0, 0,  // call  ___delayLoadHelper2@8
    0x5A,              // pop   edx
    0x59,              // pop   ecx
    0xFF, 0xE0,        // jmp   eax
};

static const uint8_t thunkARM[] = {
    0x40, 0xf2, 0x00, 0x0c, // mov.w   ip, #0 __imp_<FUNCNAME>
    0xc0, 0xf2, 0x00, 0x0c, // mov.t   ip, #0 __imp_<FUNCNAME>
    0x00, 0xf0, 0x00, 0xb8, // b.w     __tailMerge_<lib>
};

static const uint8_t tailMergeARM[] = {
    0x2d, 0xe9, 0x0f, 0x48, // push.w  {r0, r1, r2, r3, r11, lr}
    0x0d, 0xf2, 0x10, 0x0b, // addw    r11, sp, #16
    0x2d, 0xed, 0x10, 0x0b, // vpush   {d0, d1, d2, d3, d4, d5, d6, d7}
    0x61, 0x46,             // mov     r1, ip
    0x40, 0xf2, 0x00, 0x00, // mov.w   r0, #0 DELAY_IMPORT_DESCRIPTOR
    0xc0, 0xf2, 0x00, 0x00, // mov.t   r0, #0 DELAY_IMPORT_DESCRIPTOR
    0x00, 0xf0, 0x00, 0xd0, // bl      #0 __delayLoadHelper2
    0x84, 0x46,             // mov     ip, r0
    0xbd, 0xec, 0x10, 0x0b, // vpop    {d0, d1, d2, d3, d4, d5, d6, d7}
    0xbd, 0xe8, 0x0f, 0x48, // pop.w   {r0, r1, r2, r3, r11, lr}
    0x60, 0x47,             // bx      ip
};

static const uint8_t thunkARM64[] = {
    0x11, 0x00, 0x00, 0x90, // adrp    x17, #0      __imp_<FUNCNAME>
    0x31, 0x02, 0x00, 0x91, // add     x17, x17, #0 :lo12:__imp_<FUNCNAME>
    0x00, 0x00, 0x00, 0x14, // b       __tailMerge_<lib>
};

// The same, in an image with a fork object, where the pointer is within 1 MB.
static const uint8_t thunkNearARM64[] = {
    0x11, 0x00, 0x00, 0x10, // adr     x17, __imp_<FUNCNAME>
    0x00, 0x00, 0x00, 0x14, // b       __tailMerge_<lib>
};

static const uint8_t tailMergeARM64[] = {
    0xfd, 0x7b, 0xb2, 0xa9, // stp     x29, x30, [sp, #-224]!
    0xfd, 0x03, 0x00, 0x91, // mov     x29, sp
    0xe0, 0x07, 0x01, 0xa9, // stp     x0, x1, [sp, #16]
    0xe2, 0x0f, 0x02, 0xa9, // stp     x2, x3, [sp, #32]
    0xe4, 0x17, 0x03, 0xa9, // stp     x4, x5, [sp, #48]
    0xe6, 0x1f, 0x04, 0xa9, // stp     x6, x7, [sp, #64]
    0xe8, 0x2b, 0x00, 0xf9, // str     x8,     [sp, #80]
    0xe0, 0x07, 0x03, 0xad, // stp     q0, q1, [sp, #96]
    0xe2, 0x0f, 0x04, 0xad, // stp     q2, q3, [sp, #128]
    0xe4, 0x17, 0x05, 0xad, // stp     q4, q5, [sp, #160]
    0xe6, 0x1f, 0x06, 0xad, // stp     q6, q7, [sp, #192]
    0xe1, 0x03, 0x11, 0xaa, // mov     x1, x17
    0x00, 0x00, 0x00, 0x90, // adrp    x0, #0     DELAY_IMPORT_DESCRIPTOR
    0x00, 0x00, 0x00, 0x91, // add     x0, x0, #0 :lo12:DELAY_IMPORT_DESCRIPTOR
    0x02, 0x00, 0x00, 0x90, // adrp    x2, #0     __delayLoadHelper2
    0x42, 0x00, 0x00, 0x91, // add     x2, x2, #0 :lo12:__delayLoadHelper2
    0x40, 0x00, 0x3f, 0xd6, // blr     x2
    0xf0, 0x03, 0x00, 0xaa, // mov     x16, x0
    0xe6, 0x1f, 0x46, 0xad, // ldp     q6, q7, [sp, #192]
    0xe4, 0x17, 0x45, 0xad, // ldp     q4, q5, [sp, #160]
    0xe2, 0x0f, 0x44, 0xad, // ldp     q2, q3, [sp, #128]
    0xe0, 0x07, 0x43, 0xad, // ldp     q0, q1, [sp, #96]
    0xe8, 0x2b, 0x40, 0xf9, // ldr     x8,     [sp, #80]
    0xe6, 0x1f, 0x44, 0xa9, // ldp     x6, x7, [sp, #64]
    0xe4, 0x17, 0x43, 0xa9, // ldp     x4, x5, [sp, #48]
    0xe2, 0x0f, 0x42, 0xa9, // ldp     x2, x3, [sp, #32]
    0xe0, 0x07, 0x41, 0xa9, // ldp     x0, x1, [sp, #16]
    0xfd, 0x7b, 0xce, 0xa8, // ldp     x29, x30, [sp], #224
    0x00, 0x02, 0x1f, 0xd6, // br      x16
};

// Bytes that the link writes with the fields in them that name other chunks:
// a stub's code, its unwind information, or its function table entry.
class StubChunk : public NonSectionChunk {
public:
  // A field at offset that names target, with the symbol where it is one,
  // plus addend: the address relative to the image base in data, and in code
  // what the instruction there takes, an x86-64 field relative to its own
  // end, or an ARM64 adr, adrp, branch, or the add, load or store that
  // follows an adrp. A fixup with no target leaves its field as it is.
  struct Fixup {
    uint16_t offset;
    PointerUnion<Chunk *, Defined *> target;
    uint32_t addend = 0;
  };

  StubChunk(MachineTypes machine, bool code, ArrayRef<uint8_t> bytes,
            ArrayRef<Fixup> fixups)
      : bytes(bytes), fixups(fixups), machine(machine), code(code) {
    setAlignment(machine == AMD64 && code ? 1 : 4);
  }

  size_t getSize() const override { return bytes.size(); }
  MachineTypes getMachine() const override { return machine; }
  uint32_t getOutputCharacteristics() const override {
    return code ? IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE : 0;
  }

  void writeTo(uint8_t *buf) const override {
    memcpy(buf, bytes.data(), bytes.size());
    for (const Fixup &f : fixups) {
      if (f.target.isNull())
        continue;
      uint64_t s = getTarget(f);
      uint64_t p = rva + f.offset;
      uint8_t *loc = buf + f.offset;
      uint32_t insn = read32le(loc);
      if (!code)
        write32le(loc, s);
      else if (machine == AMD64)
        write32le(loc, s - p - 4);
      else if ((insn & 0x1f000000) == 0x10000000)
        applyArm64Addr(loc, s, p, insn >> 31 ? 12 : 0);
      else if ((insn & 0x7c000000) == 0x14000000) {
        write32le(loc, insn & 0xfc000000);
        applyArm64Branch26(loc, s - p);
      } else if ((insn & 0x3b000000) == 0x39000000)
        applyArm64Ldr(loc, s & 0xfff);
      else
        applyArm64Imm(loc, s & 0xfff, 0);
    }
  }

  // A stub whose first instruction is an adr, which reaches 1 MB, takes the
  // bytes of its far form where its target lies beyond.
  bool verifyRanges() override {
    return !far || isInt<21>(getTarget(fixups[0]) - rva);
  }
  uint32_t extendRanges() override {
    if (verifyRanges())
      return 0;
    uint32_t more = far->bytes.size() - bytes.size();
    bytes = far->bytes;
    fixups = far->fixups;
    far = nullptr;
    return more;
  }

  ArrayRef<uint8_t> bytes;
  StubChunk *far = nullptr;

private:
  uint64_t getTarget(const Fixup &f) const {
    if (auto *c = dyn_cast<Chunk *>(f.target))
      return c->getRVA() + f.addend;
    return cast<Defined *>(f.target)->getRVA() + f.addend;
  }

  SmallVector<Fixup, 3> fixups;
  MachineTypes machine;
  bool code;
};

static StubChunk *newStub(MachineTypes machine, bool code,
                          ArrayRef<uint8_t> bytes,
                          ArrayRef<StubChunk::Fixup> fixups = {}) {
  return make<StubChunk>(machine, code, bytes, fixups);
}

class ThunkChunkX86 : public NonSectionCodeChunk {
public:
  ThunkChunkX86(COFFLinkerContext &ctx, Defined *i, Chunk *tm)
      : imp(i), tailMerge(tm), ctx(ctx) {}

  size_t getSize() const override { return sizeof(thunkX86); }
  MachineTypes getMachine() const override { return I386; }

  void writeTo(uint8_t *buf) const override {
    memcpy(buf, thunkX86, sizeof(thunkX86));
    write32le(buf + 1, imp->getRVA() + ctx.config.imageBase);
    write32le(buf + 6, tailMerge->getRVA() - rva - 10);
  }

  void getBaserels(std::vector<Baserel> *res) override {
    res->emplace_back(rva + 1, ctx.config.machine);
  }

  Defined *imp = nullptr;
  Chunk *tailMerge = nullptr;

private:
  const COFFLinkerContext &ctx;
};

class TailMergeChunkX86 : public NonSectionCodeChunk {
public:
  TailMergeChunkX86(COFFLinkerContext &ctx, Chunk *d, Defined *h)
      : desc(d), helper(h), ctx(ctx) {}

  size_t getSize() const override { return sizeof(tailMergeX86); }
  MachineTypes getMachine() const override { return I386; }

  void writeTo(uint8_t *buf) const override {
    memcpy(buf, tailMergeX86, sizeof(tailMergeX86));
    write32le(buf + 4, desc->getRVA() + ctx.config.imageBase);
    write32le(buf + 9, helper->getRVA() - rva - 13);
  }

  void getBaserels(std::vector<Baserel> *res) override {
    res->emplace_back(rva + 4, ctx.config.machine);
  }

  Chunk *desc = nullptr;
  Defined *helper = nullptr;

private:
  const COFFLinkerContext &ctx;
};

class ThunkChunkARM : public NonSectionCodeChunk {
public:
  ThunkChunkARM(COFFLinkerContext &ctx, Defined *i, Chunk *tm)
      : imp(i), tailMerge(tm), ctx(ctx) {
    setAlignment(2);
  }

  size_t getSize() const override { return sizeof(thunkARM); }
  MachineTypes getMachine() const override { return ARMNT; }

  void writeTo(uint8_t *buf) const override {
    memcpy(buf, thunkARM, sizeof(thunkARM));
    applyMOV32T(buf + 0, imp->getRVA() + ctx.config.imageBase);
    applyBranch24T(buf + 8, tailMerge->getRVA() - rva - 12);
  }

  void getBaserels(std::vector<Baserel> *res) override {
    res->emplace_back(rva + 0, IMAGE_REL_BASED_ARM_MOV32T);
  }

  Defined *imp = nullptr;
  Chunk *tailMerge = nullptr;

private:
  const COFFLinkerContext &ctx;
};

class TailMergeChunkARM : public NonSectionCodeChunk {
public:
  TailMergeChunkARM(COFFLinkerContext &ctx, Chunk *d, Defined *h)
      : desc(d), helper(h), ctx(ctx) {
    setAlignment(2);
  }

  size_t getSize() const override { return sizeof(tailMergeARM); }
  MachineTypes getMachine() const override { return ARMNT; }

  void writeTo(uint8_t *buf) const override {
    memcpy(buf, tailMergeARM, sizeof(tailMergeARM));
    applyMOV32T(buf + 14, desc->getRVA() + ctx.config.imageBase);
    applyBranch24T(buf + 22, helper->getRVA() - rva - 26);
  }

  void getBaserels(std::vector<Baserel> *res) override {
    res->emplace_back(rva + 14, IMAGE_REL_BASED_ARM_MOV32T);
  }

  Chunk *desc = nullptr;
  Defined *helper = nullptr;

private:
  const COFFLinkerContext &ctx;
};

// A chunk for the import descriptor table.
class DelayAddressChunk : public NonSectionChunk {
public:
  // An ARM64 gate holds its thunk's address plus one, which no bound value
  // of 4-aligned code has: a guard tests its bit 0.
  explicit DelayAddressChunk(COFFLinkerContext &ctx, Chunk *c,
                             bool gate = false)
      : thunk(c), ctx(ctx), gate(gate && ctx.config.machine == ARM64) {
    setAlignment(ctx.config.wordsize);
  }
  size_t getSize() const override { return ctx.config.wordsize; }

  void writeTo(uint8_t *buf) const override {
    if (ctx.config.is64()) {
      write64le(buf, thunk->getRVA() + ctx.config.imageBase + gate);
    } else {
      uint32_t bit = 0;
      // Pointer to thumb code must have the LSB set, so adjust it.
      if (ctx.config.machine == ARMNT)
        bit = 1;
      write32le(buf, (thunk->getRVA() + ctx.config.imageBase) | bit);
    }
  }

  void getBaserels(std::vector<Baserel> *res) override {
    res->emplace_back(rva, ctx.config.machine);
  }

  Chunk *thunk;

private:
  const COFFLinkerContext &ctx;
  bool gate;
};

// Export table
// Read Microsoft PE/COFF spec 5.3 for details.

// A chunk for the export descriptor table.
class ExportDirectoryChunk : public NonSectionChunk {
public:
  ExportDirectoryChunk(int baseOrdinal, int maxOrdinal, int nameTabSize,
                       Chunk *d, Chunk *a, Chunk *n, Chunk *o)
      : baseOrdinal(baseOrdinal), maxOrdinal(maxOrdinal),
        nameTabSize(nameTabSize), dllName(d), addressTab(a), nameTab(n),
        ordinalTab(o) {}

  size_t getSize() const override {
    return sizeof(export_directory_table_entry);
  }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());

    auto *e = (export_directory_table_entry *)(buf);
    e->NameRVA = dllName->getRVA();
    e->OrdinalBase = baseOrdinal;
    e->AddressTableEntries = (maxOrdinal - baseOrdinal) + 1;
    e->NumberOfNamePointers = nameTabSize;
    e->ExportAddressTableRVA = addressTab->getRVA();
    e->NamePointerRVA = nameTab->getRVA();
    e->OrdinalTableRVA = ordinalTab->getRVA();
  }

  uint16_t baseOrdinal;
  uint16_t maxOrdinal;
  uint16_t nameTabSize;
  Chunk *dllName;
  Chunk *addressTab;
  Chunk *nameTab;
  Chunk *ordinalTab;
};

class AddressTableChunk : public NonSectionChunk {
public:
  explicit AddressTableChunk(SymbolTable &symtab, size_t baseOrdinal,
                             size_t maxOrdinal)
      : baseOrdinal(baseOrdinal), size((maxOrdinal - baseOrdinal) + 1),
        symtab(symtab) {}
  size_t getSize() const override { return size * 4; }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());
    uint64_t dirStart = symtab.edataStart->getRVA();
    uint64_t dirEnd = symtab.edataEnd->getRVA() + symtab.edataEnd->getSize();

    for (const Export &e : symtab.exports) {
      assert(e.ordinal >= baseOrdinal && "Export symbol has invalid ordinal");
      // Subtract the OrdinalBase to get the index.
      uint8_t *p = buf + (e.ordinal - baseOrdinal) * 4;
      uint32_t bit = 0;
      // Pointer to thumb code must have the LSB set, so adjust it.
      if (symtab.machine == ARMNT && !e.data)
        bit = 1;
      if (e.forwardChunk) {
        write32le(p, e.forwardChunk->getRVA() | bit);
        continue;
      }
      uint64_t rva = cast<Defined>(e.sym)->getRVA();
      assert(rva != 0 && "Exported symbol unmapped");
      // The loader reads an address inside the export directory as a
      // forwarder string.
      if (rva - dirStart < dirEnd - dirStart)
        Err(symtab.ctx) << "exported symbol " << e.exportName
                        << " is inside the export directory";
      write32le(p, rva | bit);
    }
  }

private:
  size_t baseOrdinal;
  size_t size;
  const SymbolTable &symtab;
};

class NamePointersChunk : public NonSectionChunk {
public:
  explicit NamePointersChunk(std::vector<Chunk *> &v) : chunks(v) {}
  size_t getSize() const override { return chunks.size() * 4; }

  void writeTo(uint8_t *buf) const override {
    for (Chunk *c : chunks) {
      write32le(buf, c->getRVA());
      buf += 4;
    }
  }

private:
  std::vector<Chunk *> chunks;
};

class ExportOrdinalChunk : public NonSectionChunk {
public:
  explicit ExportOrdinalChunk(const SymbolTable &symtab, size_t baseOrdinal,
                              size_t tableSize)
      : baseOrdinal(baseOrdinal), size(tableSize), symtab(symtab) {}
  size_t getSize() const override { return size * 2; }

  void writeTo(uint8_t *buf) const override {
    for (const Export &e : symtab.exports) {
      if (e.noname)
        continue;
      assert(e.ordinal >= baseOrdinal && "Export symbol has invalid ordinal");
      // This table stores unbiased indices, so subtract OrdinalBase.
      write16le(buf, e.ordinal - baseOrdinal);
      buf += 2;
    }
  }

private:
  size_t baseOrdinal;
  size_t size;
  const SymbolTable &symtab;
};

} // anonymous namespace

void IdataContents::create(COFFLinkerContext &ctx) {
  std::vector<std::vector<DefinedImportData *>> v = binImports(ctx, imports);

  // In hybrid images, EC and native code are usually very similar,
  // resulting in a highly similar set of imported symbols. Consequently,
  // their import tables can be shared, with ARM64X relocations handling any
  // differences. Identify matching import files used by EC and native code, and
  // merge them into a single hybrid import entry.
  if (ctx.hybridSymtab) {
    for (std::vector<DefinedImportData *> &syms : v) {
      std::vector<DefinedImportData *> hybridSyms;
      ImportFile *prev = nullptr;
      for (DefinedImportData *sym : syms) {
        ImportFile *file = sym->file;
        // At this stage, symbols are sorted by base name, ensuring that
        // compatible import files, if present, are adjacent. Check if the
        // current symbol's file imports the same symbol as the previously added
        // one (if any and if it was not already merged). Additionally, verify
        // that one of them is native while the other is EC. In rare cases,
        // separate matching import entries may exist within the same namespace,
        // which cannot be merged.
        if (!prev || file->isEC() == prev->isEC() ||
            !file->isSameImport(prev)) {
          // We can't merge the import file, just add it to hybridSyms
          // and set prev to its file so that we can try to match the next
          // symbol.
          hybridSyms.push_back(sym);
          prev = file;
          continue;
        }

        // A matching symbol may appear in syms in any order. The native variant
        // exposes a subset of EC symbols and chunks, so always use the EC
        // variant as the hybrid import file. If the native file was already
        // added, replace it with the EC symbol in hybridSyms. Otherwise, the EC
        // variant is already pushed, so we can simply merge it.
        if (file->isEC()) {
          hybridSyms.pop_back();
          hybridSyms.push_back(sym);
        }

        // Merge import files by storing their hybrid form in the corresponding
        // file class.
        prev->hybridFile = file;
        file->hybridFile = prev;
        prev = nullptr; // A hybrid import file cannot be merged again.
      }

      // Sort symbols by type: native-only files first, followed by merged
      // hybrid files, and then EC-only files.
      llvm::stable_sort(hybridSyms,
                        [](DefinedImportData *a, DefinedImportData *b) {
                          if (a->file->hybridFile)
                            return !b->file->hybridFile && b->file->isEC();
                          return !a->file->isEC() && b->file->isEC();
                        });
      syms = std::move(hybridSyms);
    }
  }

  // Each run of in-place slots is the address table of a descriptor of its
  // own, with a lookup table of its own that shares the imports' hints and
  // names. The loader looks a descriptor's DLL up as it reaches it, and a DLL
  // that this load brings in is found only once it is mapped, so the runs'
  // descriptors follow every DLL's own, in the order of the DLLs; a DLL with
  // none of its own keeps the first of its runs in its place.
  StringMap<size_t> dllIndex;
  for (size_t i = 0, e = v.size(); i != e; ++i)
    dllIndex[v[i][0]->getDLLName().lower()] = i;
  std::vector<std::vector<const ImportSlot *>> runs(v.size());
  for (const ImportSlot &s : ctx.importSlots)
    if (s.startsRun)
      runs[dllIndex.at(s.sym->getDLLName().lower())].push_back(&s);
  // With runs, every descriptor's lookup table is in one pool.
  auto *pool = ctx.importSlots.empty() ? nullptr : make<LookupPoolChunk>();
  DenseMap<ImportFile *, uint64_t> keys;
  std::vector<std::pair<ImportDirectoryChunk *, StringRef>> tables;
  std::string table;
  auto addKey = [&](DefinedImportData *s) {
    auto [it, inserted] = keys.try_emplace(s->file, pool->imports.size() + 1);
    if (inserted)
      pool->imports.push_back(s);
    char key[8];
    write64le(key, it->second);
    table.append(key, 8);
  };
  auto addTable = [&](ImportDirectoryChunk *dir) {
    table.append(8, 0);
    StringRef saved = saver().save(StringRef(table));
    table.clear();
    pool->builder.add(saved);
    tables.push_back({dir, saved});
  };
  auto addRun = [&](Chunk *dllName, const ImportSlot *s) {
    auto *dir = make<ImportDirectoryChunk>(dllName);
    dir->addressTab = s->chunk;
    dir->addressOffset = s->offset;
    const ImportSlot *end = ctx.importSlots.data() + ctx.importSlots.size();
    do
      addKey(s->sym);
    while (++s != end && !s->startsRun);
    addTable(dir);
    dirs.push_back(dir);
  };

  // Create .idata contents for each DLL.
  for (size_t dll = 0; dll != v.size(); ++dll) {
    std::vector<DefinedImportData *> &syms = v[dll];
    dllNames.push_back(make<StringChunk>(syms[0]->getDLLName()));
    // An import held only in slots has no entry in the DLL's tables, only
    // the hint and name that its slots' lookup entries name.
    llvm::erase_if(syms, [&](DefinedImportData *s) {
      if (!s->file->slotOnly)
        return false;
      if (!s->getExternalName().empty()) {
        s->file->hintName =
            make<HintNameChunk>(s->getExternalName(), s->getOrdinal());
        hints.push_back(s->file->hintName);
      }
      return true;
    });
    if (syms.empty()) {
      addRun(dllNames[dll], runs[dll][0]);
      runs[dll].erase(runs[dll].begin());
      continue;
    }
    // Create lookup and address tables. If they have external names,
    // we need to create hintName chunks to store the names.
    // If they don't (if they are import-by-ordinals), we store only
    // ordinal values to the table.
    size_t base = lookups.size();
    Chunk *lookupsTerminator = nullptr, *addressesTerminator = nullptr;
    uint32_t nativeOnly = 0;
    for (DefinedImportData *s : syms) {
      uint16_t ord = s->getOrdinal();
      HintNameChunk *hintChunk = nullptr;
      Chunk *lookupsChunk, *addressesChunk;

      if (s->getExternalName().empty()) {
        lookupsChunk = make<OrdinalOnlyChunk>(ctx, ord);
        addressesChunk = make<OrdinalOnlyChunk>(ctx, ord);
      } else {
        hintChunk = make<HintNameChunk>(s->getExternalName(), ord);
        lookupsChunk = make<LookupChunk>(ctx, hintChunk);
        addressesChunk = make<LookupChunk>(ctx, hintChunk);
        hints.push_back(hintChunk);
        s->file->hintName = hintChunk;
      }

      // Detect the first EC-only import in the hybrid IAT. Emit null chunk
      // as a terminator for the native view, and add an ARM64X relocation to
      // replace it with the correct import for the EC view.
      //
      // Additionally, for MSVC compatibility, store the lookup and address
      // chunks and append them at the end of EC-only imports, where a null
      // terminator chunk would typically be placed. Since they appear after
      // the native terminator, they will be ignored in the native view.
      // In the EC view, they should act as terminators, so emit ZEROFILL
      // relocations overriding them.
      if (ctx.config.machine == ARM64X && !lookupsTerminator &&
          s->file->isEC() && !s->file->hybridFile) {
        lookupsTerminator = lookupsChunk;
        addressesTerminator = addressesChunk;
        lookupsChunk = make<NullChunk>(ctx);
        addressesChunk = make<NullChunk>(ctx);

        Arm64XRelocVal relocVal = hintChunk;
        if (!hintChunk)
          relocVal = (1ULL << 63) | ord;
        ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE,
                               sizeof(uint64_t), lookupsChunk, relocVal);
        ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE,
                               sizeof(uint64_t), addressesChunk, relocVal);
        ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL,
                               sizeof(uint64_t), lookupsTerminator);
        ctx.dynamicRelocs->add(IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL,
                               sizeof(uint64_t), addressesTerminator);
      }

      lookups.push_back(lookupsChunk);
      addresses.push_back(addressesChunk);

      if (s->file->isEC()) {
        auto chunk = make<AuxImportChunk>(s->file);
        auxIat.push_back(chunk);
        s->file->impECSym->setLocation(chunk);

        chunk = make<AuxImportChunk>(s->file);
        auxIatCopy.push_back(chunk);
        s->file->auxImpCopySym->setLocation(chunk);
      } else if (ctx.hybridSymtab) {
        // Fill the auxiliary IAT with null chunks for native-only imports.
        auxIat.push_back(make<NullChunk>(ctx));
        auxIatCopy.push_back(make<NullChunk>(ctx));
        ++nativeOnly;
      }
    }
    // Terminate with null values.
    lookups.push_back(lookupsTerminator ? lookupsTerminator
                                        : make<NullChunk>(ctx));
    addresses.push_back(addressesTerminator ? addressesTerminator
                                            : make<NullChunk>(ctx));
    if (ctx.symtab.isEC()) {
      auxIat.push_back(make<NullChunk>(ctx));
      auxIatCopy.push_back(make<NullChunk>(ctx));
    }

    for (int i = 0, e = syms.size(); i < e; ++i) {
      syms[i]->setLocation(addresses[base + i]);
      if (syms[i]->file->hybridFile)
        syms[i]->file->hybridFile->impSym->setLocation(addresses[base + i]);
    }

    // Create the import table header.
    auto *dir = make<ImportDirectoryChunk>(dllNames.back());

    if (ctx.hybridSymtab && nativeOnly) {
      if (ctx.config.machine != ARM64X)
        // On pure ARM64EC targets, skip native-only imports in the import
        // directory.
        base += nativeOnly;
      else if (nativeOnly) {
        // If native-only imports exist, they will appear as a prefix to all
        // imports. Emit ARM64X relocations to skip them in the EC view.
        ctx.dynamicRelocs->add(
            IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA, 0,
            Arm64XRelocVal(
                dir, offsetof(ImportDirectoryTableEntry, ImportLookupTableRVA)),
            nativeOnly * sizeof(uint64_t));
        ctx.dynamicRelocs->add(
            IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA, 0,
            Arm64XRelocVal(dir, offsetof(ImportDirectoryTableEntry,
                                         ImportAddressTableRVA)),
            nativeOnly * sizeof(uint64_t));
      }
    }

    dir->lookupTab = lookups[base];
    dir->addressTab = addresses[base];
    dirs.push_back(dir);
    if (pool) {
      for (DefinedImportData *s : syms)
        addKey(s);
      addTable(dir);
    }
  }

  for (size_t dll = 0; dll != v.size(); ++dll)
    for (const ImportSlot *s : runs[dll])
      addRun(dllNames[dll], s);
  if (pool) {
    pool->builder.finalize();
    for (auto [dir, saved] : tables) {
      dir->lookupTab = pool;
      dir->lookupOffset = pool->builder.getOffset(saved);
    }
    lookups.assign(1, pool);
  }
  // Add null terminator.
  dirs.push_back(make<NullChunk>(sizeof(ImportDirectoryTableEntry), 4));
}

std::vector<Chunk *> DelayLoadContents::getChunks() {
  std::vector<Chunk *> v;
  v.insert(v.end(), dirs.begin(), dirs.end());
  v.insert(v.end(), names.begin(), names.end());
  v.insert(v.end(), hintNames.begin(), hintNames.end());
  v.insert(v.end(), dllNames.begin(), dllNames.end());
  v.insert(v.end(), unloads.begin(), unloads.end());
  v.insert(v.end(), lists.begin(), lists.end());
  return v;
}

// The delay-load IAT's placement, decided as link.exe does. Only a GUI or
// console image without /nooptdidat protects it. A helper whose own object
// defines __MSSafeDelayLoadIAT, for a subsystem above 6.3, has it protected
// beside the IAT, unless /didatownsection puts it alone in a writable .didat;
// otherwise /guard:cf puts it there, and else it is unprotected.
DelayIatPlacement getDelayIatPlacement(COFFLinkerContext &ctx) {
  Configuration &config = ctx.config;
  bool imports = llvm::any_of(ctx.importFileInstances, [&](ImportFile *f) {
    return f->live && config.delayLoads.contains(StringRef(f->dllName).lower());
  });
  if (!imports || config.noOptDidat ||
      (config.subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI &&
       config.subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI))
    return DelayIatPlacement::Data;
  bool safe = true;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (!symtab.delayLoadHelper)
      return;
    auto *helper = dyn_cast<DefinedCOFF>(symtab.delayLoadHelper);
    auto *sym = dyn_cast_or_null<DefinedCOFF>(
        symtab.findUnderscore("__MSSafeDelayLoadIAT"));
    safe &= helper && sym && helper->getFile() == sym->getFile();
  });
  if (safe &&
      std::make_pair(config.majorSubsystemVersion,
                     config.minorSubsystemVersion) > std::make_pair(6u, 3u))
    return config.didatOwnSection ? DelayIatPlacement::Didat
                                  : DelayIatPlacement::Idata;
  return config.guardCF != GuardCFLevel::Off ? DelayIatPlacement::Didat
                                             : DelayIatPlacement::Data;
}

// push h, and for a replay push rax; then for each gate, lea h, [thunk];
// not h; mov r, [gate]; lea rcx, [r+h+1]: rcx is zero exactly while the
// gate holds the thunk, and jrcxz to the call of the gate's ensure stub,
// which keeps every register and the flags and goes back to the next gate.
// Then the value's mov or lea, pop h and ret; or pop rax, pop rcx, and a jump
// out of the function to the instruction and a ret. h is rcx, or rax where
// r is rcx; REX2 encodes r16 to r31.
void DelayGuardChunk::writeTo(uint8_t *buf) const {
  if (!called)
    return;
  uint8_t *p = buf, h = reg == 1 ? 0 : 1;
  SmallVector<uint8_t *, 4> backs;
  auto put = [&](std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes)
      *p++ = b;
  };
  auto rip = [&](Defined *to, int32_t addend = 0, unsigned after = 0) {
    write32le(p, to->getRVA() + addend - (getRVA() + (p - buf) + 4 + after));
    p += 4;
  };
  // REX.W or REX2 with r in the modrm reg field (shift 2) or base (shift 0).
  auto prefix = [&](unsigned shift) {
    uint8_t bits = (reg >> 3 & 1) << shift | (reg >> 4 & 1) << (shift + 4);
    if (reg >= 16)
      put({0xd5, uint8_t(0x08 | bits)});
    else
      put({uint8_t(0x48 | bits)});
  };
  auto load = [&](uint8_t opcode, Defined *from, int32_t addend = 0) {
    prefix(2);
    put({opcode, uint8_t(0x05 | (reg & 7) << 3)});
    rip(from, addend);
  };
  put({uint8_t(0x50 | h)});
  if (!insn.empty())
    put({0x50});
  for (const Gate &g : gates) {
    put({0x48, 0x8d, uint8_t(0x05 | h << 3)});
    rip(g.thunk);
    put({0x48, 0xf7, uint8_t(0xd0 | h)});
    load(0x8b, g.gate);
    prefix(0);
    put({0x8d, 0x4c, uint8_t(h << 3 | (reg & 7)), 0x01, 0xe3, 0});
    backs.push_back(p);
  }
  if (insn.empty()) {
    load(address ? 0x8d : 0x8b, value, addend);
    put({uint8_t(0x58 | h), 0xc3});
  } else {
    put({0x58, 0x59, 0xeb, uint8_t(7 * gates.size())});
  }
  for (auto [g, back] : llvm::zip(gates, backs)) {
    back[-1] = p - back;
    put({0xe8});
    rip(g.ensure);
    put({0xeb, uint8_t(back - (p + 2))});
  }
  if (insn.empty())
    return;
  memcpy(p, insn.data(), insn.size());
  p += before;
  rip(value, addend, insn.size() - before - 4);
  p = buf + getSize() - 1;
  *p = 0xc3;
}

// The gate's names of a delay-loaded DLL, which are defined where the DLL's
// tables are made.
static Defined *getGateName(COFFLinkerContext &ctx, const char *prefix,
                            StringRef dll) {
  std::string n = (prefix + dll).str();
  Symbol *s = ctx.symtab.find(n);
  return cast<Defined>(s ? s
                         : ctx.symtab.addSynthetic(saver().save(n), nullptr));
}

// The gates of the DLLs whose words value may reach: an import's entry
// its DLL's, and a delay-bound section, or a local import's pointer to one,
// those of the DLLs whose words it holds or reaches.
static SmallVector<DelayGuardChunk::Gate, 1> getGates(COFFLinkerContext &ctx,
                                                      Defined *value) {
  SmallSetVector<StringRef, 1> dlls;
  if (auto *cell = dyn_cast<DefinedImportData>(value))
    dlls.insert(saver().save(cell->getDLLName().lower()));
  else if (auto *li = dyn_cast<DefinedLocalImport>(value))
    dlls = ctx.delayBound.lookup(li->getTarget()->getChunk()).dlls;
  else
    dlls = ctx.delayBound.lookup(value->getChunk()).dlls;
  SmallVector<DelayGuardChunk::Gate, 1> gates;
  for (StringRef dll : dlls)
    gates.push_back({getGateName(ctx, "__gate_", dll),
                     getGateName(ctx, "__gate_thunk_", dll),
                     getGateName(ctx, "__ensure_", dll)});
  return gates;
}

Defined *getDelayGuard(COFFLinkerContext &ctx, const DelayGuardSite &g,
                       bool called) {
  // A replay loads the gates into rax.
  unsigned reg = std::max<int8_t>(g.reg, 0);
  Defined *&leaf = ctx.delayGuards[{
      g.value, g.addend, reg | g.address << 8 | g.before << 9, g.insn}];
  if (!leaf) {
    SmallVector<DelayGuardChunk::Gate, 1> gates = getGates(ctx, g.value);
    // A jrcxz reaches the call of its gate's ensure stub over five tests.
    if (gates.size() > 5) {
      errAt(*g.chunk, g.chunk->getRelocs()[g.index].VirtualAddress)
          << "reaches " << g.value->getName()
          << ", which holds the addresses of functions of more than five "
             "delay-loaded DLLs";
      gates.resize(5);
    }
    leaf = make<DefinedSynthetic>(
        "__delay_guard", make<DelayGuardChunk>(g, reg, std::move(gates)));
  }
  toDelayGuard(leaf)->called |= called;
  return leaf;
}

// For each gate, adrp and ldr of it and tbz of its bit 0 to the next; adr of
// the site, stp x30 and it, bl of the ensure stub or its veneer, ldr x30.
// Then the site's adrp of target, b back past it, and the veneers.
void DelayGuardA64Chunk::writeTo(uint8_t *buf) const {
  const Relocation &r = sc->getScanned()->relocs[index];
  uint64_t site = sc->getRVA() + sc->getOutputOffset(r.offset), p = getRVA();
  uint64_t back =
      site + (r.expr == R_GUARD ? gates.size() * (12 + 4 * far) + 4 : 4);
  auto put = [&](uint32_t insn) {
    write32le(buf, insn);
    buf += 4, p += 4;
  };
  auto adrp = [&](uint32_t insn, uint64_t s, bool add) {
    put(insn);
    applyArm64Addr(buf - 4, s, p - 4, 12);
    if (add)
      put(0x91000000 | reg * 33), applyArm64Imm(buf - 4, s & 0xfff, 0);
  };
  for (const DelayGuardChunk::Gate &g : gates) {
    adrp(0x90000000 | reg, g.gate->getRVA(), false);
    put(0xf9400000 | reg * 33);
    applyArm64Ldr(buf - 4, g.gate->getRVA() & 0xfff);
    put(0x36000000 | (getBlock() / 4 - 2) << 5 | reg);
    if (farAdr) {
      adrp(0x90000000 | reg, site, true);
    } else {
      if (!isInt<21>(site - p))
        Err(sc->file->symtab.ctx) << sc->getSectionName() << " of " << sc->file
                                  << " is over 1 MB, beyond its guard's adr";
      put(0x10000000 | reg), applyArm64Addr(buf - 4, site, p - 4, 0);
    }
    put(0xa9bf03fe | reg << 10);
    put(0x94000000);
    applyArm64Branch26(buf - 4,
                       (farCall ? getRVA() + getSize() - 12 * (gates.end() - &g)
                                : g.ensure->getRVA()) -
                           (p - 4));
    put(0xf84107fe);
  }
  adrp(read32le(sc->getContents().data() + r.offset), target->getRVA(), false);
  put(0x14000000);
  applyArm64Branch26(buf - 4, back - (p - 4));
  for (const DelayGuardChunk::Gate &g :
       farCall ? gates : ArrayRef<DelayGuardChunk::Gate>()) {
    adrp(0x90000000 | reg, g.ensure->getRVA(), true);
    put(0xd61f0000 | reg << 5);
  }
}

// Whether each block's bl reaches its ensure stub from anywhere in the guard.
bool DelayGuardA64Chunk::verifyRanges() {
  return farCall || llvm::all_of(gates, [&](const DelayGuardChunk::Gate &g) {
           int64_t s = g.ensure->getRVA();
           return isInt<28>(s - rva) && isInt<28>(s - (rva + getSize()));
         });
}

uint32_t DelayGuardA64Chunk::extendRanges() {
  if (verifyRanges())
    return 0;
  farCall = true;
  return 12 * gates.size();
}


bool namesDelayGate(COFFLinkerContext &ctx, StringRef dll, bool define) {
  bool named = false;
  for (const char *prefix : {"__ensure_", "__gate_", "__gate_thunk_"}) {
    std::string name = (prefix + dll).str();
    Symbol *s = ctx.symtab.find(name);
    // Code that reaches the name through an import pointer gets a local one.
    if (define &&
        (s ? isa<Undefined>(s) : ctx.symtab.find("__imp_" + name) != nullptr))
      s = ctx.symtab.addSynthetic(saver().save(name), nullptr);
    named |= s != nullptr;
  }
  return named;
}

void dropBuiltin(Symbol *sym) {
  // A function that shares its section with others stays with them.
  if (auto *d = dyn_cast_or_null<DefinedRegular>(sym);
      d && d->getChunk()->isCOMDAT()) {
    d->getChunk()->live = false;
    for (SectionChunk &c : d->getChunk()->children())
      c.live = false;
  }
}

void classifyImports(COFFLinkerContext &ctx) {
  for (ImportFile *f : ctx.importFileInstances) {
    // A DLL binds what it imports from an executable in the process's own,
    // and an import a strong reference to its pointer names is no weak one.
    bool host = ctx.config.dll && ctx.driver.importsFromExecutable(f);
    bool weak = f->binding == ImportFile::Weak && !f->impSym->strongImport;
    f->binding = ImportFile::Binding(host | weak << 1);
    bool delayed =
        ctx.config.delayLoads.contains(StringRef(f->dllName).lower());
    f->delayLoaded = !f->binding && delayed;
    if (f->live && weak && delayed)
      Err(ctx) << "/delayload:" << f->dllName << ": only weak references name "
               << f->externalName
               << ", which this would make a dependency; refer to it strongly";
  }
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(
            isa_and_nonnull<DefinedImportThunk>(e.sym)
                ? cast<DefinedImportThunk>(e.sym)->wrappedSym
                : e.sym);
        imp && (imp->file->binding & ImportFile::Weak))
      Err(ctx) << "/export:" << e.name << ": only weak references name "
               << imp->getExternalName() << ", imported from "
               << imp->getDLLName()
               << ", which this would make a dependency; refer to it strongly";
}

void selectFill(COFFLinkerContext &ctx,
                SmallVectorImpl<SectionChunk *> &kept) {
  // A sealed value no live section reads is no part of the image, and any
  // other keeps its import's entry, or the pointer to a record of its own.
  llvm::erase_if(ctx.fill.words, [](const FillWord &w) {
    if (w.kind == FillWord::Address)
      return false;
    if (!cast<LocalImportChunk>(w.chunk)->isLive())
      return true;
    if (auto *li = dyn_cast<DefinedLocalImport>(w.source)) {
      li->getChunk()->uses |= LocalImportChunk::Keep;
      return false;
    }
    ImportFile *file = cast<DefinedImportData>(w.source)->file;
    file->live = true;
    file->uses |= ImportFile::Keep;
    return false;
  });
  ctx.symtab.selectPlaceWord();
  ctx.symtab.bindOverrideCells();
  // The pointer of an import that the fill binds by name is a word of
  // .sealed, which the fill writes in place of the entry.
  bool byName = false;
  for (ImportFile *f : ctx.importFileInstances)
    if (f->live && f->binding) {
      if (f->externalName.empty())
        Err(ctx) << f->impSym->getName() << ", imported from " << f->dllName
                 << " by ordinal, is bound by name as the image starts; "
                    "import it by name";
      f->uses |= ImportFile::Keep;
      f->location = make<EmptyChunk>(8);
      f->location->setAlignment(8);
      ctx.fill.words.push_back({f->location, 0, f->impSym, 0, FillWord::Import});
      byName = true;
    }
  if (ctx.fill.empty())
    return;
  // A word of a thread-local template, which each thread copies: the image's
  // is read-only and copied by its runtime once the fill has written it; the
  // loader copies its own into threads before the fill runs.
  bool registers = ctx.symtab.placeWord && ctx.symtab.placeWord->registers;
  for (FillWord &w : ctx.fill.words) {
    auto *sc = dyn_cast_or_null<SectionChunk>(w.chunk);
    if (!sc || sc->getSectionName().split('$').first != ".tls")
      continue;
    // The runtime copies an executable's template into the threads that
    // start before its fill, which registers the template again.
    if (sc->file->tlsModel) {
      if (!ctx.config.dll && !registers)
        reportFillWord(ctx, w)
            << ", in the thread-local template of an executable, which its "
               "fill registers again through "
            << COFF::TP::RegisterImport << ", which the image does not have";
      continue;
    }
    reportFillWord(ctx, w) << ", in the loader's thread-local template, "
                              "which threads copy before the image's fill runs";
  }
  // The runtime's entry point, which runs the fill and then the entry the
  // image was linked with, is the image's. Only one that binds imports by
  // name holds the search, or a DLL with a vague cell, which searches where
  // it is loaded after the process starts.
  bool imports =
      byName || (ctx.config.dll && llvm::any_of(ctx.fill.words, isVagueCell));
  std::string name = ctx.config.dll ? "__llvm_fill_dll" : "__llvm_fill_exe";
  if (imports)
    name += "_imports";
  Symbol *fill = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(name));
  if (!fill) {
    const FillWord &w = ctx.fill.words[0];
    if (w.kind == FillWord::Cell || !isa<DefinedImportData>(w.source)) {
      Err(ctx) << "the image writes its cells as it starts, which needs "
               << name << "; link the builtins library";
      return;
    }
    COFFSyncStream e = reportFillWord(ctx, w);
    if (w.kind != FillWord::Import)
      e << ": the image computes it as it starts";
    e << ", which needs " << name << "; link the builtins library";
    return;
  }
  // The fill opens read-only memory, so nothing but the loader calls it.
  for (ObjFile *file : ctx.objFileInstances)
    if (file != fill->getFile() && llvm::is_contained(file->getSymbols(), fill))
      Err(ctx) << file << ": refers to " << fill->getName()
               << ", which the loader calls, and nothing else may name";
  ctx.symtab.fill = fill;
  // It ends with a jump to the image's own entry, or with no entry to
  // __llvm_fill_true.
  Symbol *entry = ctx.config.noEntry ? ctx.symtab.find("__llvm_fill_true")
                                     : ctx.symtab.entry;
  if (!isa_and_nonnull<Defined>(entry)) {
    Err(ctx) << name << " needs __llvm_fill_true; link the builtins library";
    return;
  }
  ctx.symtab.find("__llvm_fill_target")
      ->replaceKeepingName(entry, sizeof(SymbolUnion));
  ctx.symtab.entry = fill;
  ctx.config.noEntry = false;
  // The loader runs an image's TLS callbacks before its entry, so where the
  // image has any, the fill runs as the first of them, after only a -static
  // runtime's founding. An executable with a TLS directory always runs it
  // there: its callbacks run once every DLL the process starts with is
  // initialized, and before any thread those DLLs start runs.
  SmallVector<Symbol *, 2> roots = {fill};
  if ((!ctx.config.dll &&
       isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("_tls_used"))) ||
      llvm::any_of(ctx.objFileInstances, [](ObjFile *f) {
        return llvm::any_of(f->getChunks(), [](Chunk *c) {
          auto *sc = dyn_cast<SectionChunk>(c);
          return sc && sc->live && !sc->getRelocs().empty() &&
                 sc->getSectionName().starts_with(".CRT$XL");
        });
      })) {
    StringRef tls = imports ? "__llvm_fill_imports_tls" : "__llvm_fill_tls";
    roots.push_back(ctx.symtab.find(tls));
    if (!isa_and_nonnull<Defined>(roots.back())) {
      Err(ctx) << "the image's fill runs before its TLS callbacks, which "
                  "needs "
               << tls << "; link the builtins library";
      return;
    }
  }
  // A callback the link did not select never runs, though a link that
  // collects no garbage keeps every section: the fill of an image that binds
  // imports by name, run without its search, would leave them unwritten.
  for (StringRef tls : {"__llvm_fill_tls", "__llvm_fill_imports_tls"})
    if (auto *d = dyn_cast_or_null<DefinedRegular>(ctx.symtab.find(tls));
        d && !llvm::is_contained(roots, d))
      for (SectionChunk &c : d->getChunk()->children())
        c.live = false;
  markLive(ctx, roots, kept);
}

void selectDelayBoundImports(COFFLinkerContext &ctx) {
  Configuration &config = ctx.config;
  if (!ctx.symtab.delayResolve)
    return;
  // Such an address is that of the image's thunk, as MSVC has it, where the
  // loader's unprotected path binds the tables, one word a call; where the
  // image refers to the function that unloads a DLL and has the tables to
  // restore; where the user asks for it; and in an image without an object
  // for Windows Itanium or NT-POSIX, or a hybrid one, which load no address
  // from an import's entry.
  auto *unload = dyn_cast_or_null<Defined>(
      ctx.symtab.findUnderscore("__FUnloadDelayLoadedDLL2"));
  bool keeps = !ctx.hasForkObject || ctx.hybridSymtab ||
               isArm64EC(config.machine) ||
               getDelayIatPlacement(ctx) == DelayIatPlacement::Data ||
               (config.delayUnload && unload && unload->isLive());
  bool any = false;
  for (ImportFile *file : ctx.importFileInstances) {
    if (file->observed &&
        (keeps || !file->live ||
         config.delayNoCanonical.contains(StringRef(file->dllName).lower())))
      file->observed = false;
    any |= file->observed;
  }
  for (const auto &dll : config.delayLoads)
    any |= namesDelayGate(ctx, dll.getKey());
  // The runtime's function is part of the image only where a stub calls it.
  dropBuiltin(ctx.symtab.delayResolve);
  SmallVector<SectionChunk *, 8> kept;
  if (any)
    markLive(ctx, ctx.symtab.delayResolve, kept);
}

// Returns the number of words from offset o of c, where a run ends, to the
// first zero word, which ends the run's table as the loader counts it: a
// word of zero bytes that no relocation writes. Where c holds none, pad is
// the number of zero bytes that are to follow it.
static uint32_t getRunTail(const SectionChunk *c, uint32_t o, uint32_t &pad) {
  ArrayRef<uint8_t> b = c->getContents();
  SmallVector<uint32_t, 8> relocated;
  for (const coff_relocation &rel : c->getRelocs())
    if (uint64_t(rel.VirtualAddress) + 8 > o)
      relocated.push_back(rel.VirtualAddress);
  pad = 0;
  for (uint32_t n = 0;; o += 8, ++n) {
    if (o >= b.size()) {
      pad = o + 8 - b.size();
      return n;
    }
    if (o + 8 > b.size())
      continue;
    if (llvm::all_of(b.slice(o, 8), [](uint8_t x) { return !x; }) &&
        llvm::none_of(relocated, [&](uint32_t r) {
          return uint64_t(r) < o + 8 && uint64_t(r) + 8 > o;
        }))
      return n;
  }
}

bool DelayLoadContents::endsSection(const ImportSlot &last) {
  uint32_t pad;
  getRunTail(last.chunk, last.offset + 8, pad);
  return pad;
}

void DelayLoadContents::placeWords(std::vector<ImportSlot> readOnly,
                                   size_t gathered,
                                   std::vector<ImportSlot> writable) {
  runs = std::move(readOnly);
  words = std::move(writable);
  // Words of one DLL a word apart are a run, which goes on into the next of
  // the gathered sections where that follows the last word of this one with
  // no padding, and this one holds no other run, whose table would go on
  // with it.
  size_t runsInSection = 0;
  for (size_t i = 0, e = runs.size(); i != e; ++i) {
    ImportSlot &s = runs[i];
    const ImportSlot *prev = i ? &runs[i - 1] : nullptr;
    s.startsRun = !continuesRun(prev, s, runsInSection == 1 && i < gathered);
    runsInSection =
        prev && prev->chunk == s.chunk ? runsInSection + s.startsRun : 1;
  }
  // A section that holds no zero word after its last run gets one, unless
  // the run goes on. The gathered sections are on pages of their own, which
  // the loader opens while it binds a run.
  for (size_t i = 0, e = runs.size(); i != e; ++i) {
    SectionChunk *c = runs[i].chunk;
    if (i < gathered && (!i || runs[i - 1].chunk != c))
      runChunks.push_back(c);
    if (i + 1 != e && runs[i + 1].chunk == c)
      continue;
    uint32_t pad;
    getRunTail(c, runs[i].offset + 8, pad);
    if (!pad || (i + 1 != e && !runs[i + 1].startsRun))
      continue;
    runPads[c] = make<NullChunk>(pad, 1);
    if (i < gathered)
      runChunks.push_back(runPads[c]);
    else
      placedPads.push_back({c, runPads[c]});
  }
  if (!runChunks.empty()) {
    runChunks[0]->setAlignment(std::max(0x1000u, runChunks[0]->getAlignment()));
    runChunks.push_back(make<NullChunk>(0, 0x1000));
  }
}

void DelayLoadContents::checkWords() {
  auto report = [&](const ImportSlot &s) {
    COFFSyncStream err = Err(ctx);
    err << s.chunk->file << ": the address of "
        << s.sym->getName().substr(strlen("__imp_")) << ", delay-loaded from "
        << s.sym->getDLLName() << ", at offset 0x" << Twine::utohexstr(s.offset)
        << " of " << s.chunk->getSectionName();
    return err;
  };
  const uint32_t writable = IMAGE_SCN_MEM_WRITE;
  uint64_t last = 0;
  for (size_t i = 0, e = runs.size(); i != e; ++i) {
    const ImportSlot &s = runs[i];
    OutputSection *os = ctx.getOutputSection(s.chunk);
    uint64_t rva = s.chunk->getRVA() + s.offset;
    Chunk *pad = runPads.lookup(s.chunk);
    if (os->header.Characteristics & (writable | IMAGE_SCN_MEM_EXECUTE))
      report(s) << " is in section " << os->name
                << ", which is not read-only data, where the loader cannot "
                   "bind it in place; keep its section apart from /merge and "
                   "/section";
    else if (!s.startsRun && rva != last + 8)
      report(s) << " does not follow the word before it in its run";
    else if (pad && (i + 1 == e || runs[i + 1].chunk != s.chunk) &&
             pad->getRVA() != s.chunk->getRVA() + s.chunk->getSize())
      report(s) << " ends a run that the zero word after its section does "
                   "not follow";
    last = rva;
  }
  for (const ImportSlot &s : words)
    if (OutputSection *os = ctx.getOutputSection(s.chunk);
        !(os->header.Characteristics & writable))
      report(s) << " is in section " << os->name
                << ", which is read-only, where the runtime cannot write it; "
                   "keep its section apart from /merge and /section";
}

uint64_t DelayLoadContents::getDirSize() {
  return dirs.size() * sizeof(delay_import_directory_table_entry);
}

void DelayLoadContents::create() {
  std::vector<std::vector<DefinedImportData *>> v = binImports(ctx, imports);
  // The DLLs that have a gate, each with what its further descriptors name.
  struct Gated {
    Chunk *name;
    Chunk *handle;
    DelayListChunk *list;
  };
  StringMap<Gated> gates;
  StringSet<> hasWords;
  for (const std::vector<ImportSlot> *w : {&runs, &words})
    for (const ImportSlot &s : *w)
      hasWords.insert(s.sym->getDLLName().lower());
  auto newName = [&](DefinedImportData *s) -> Chunk * {
    if (s->getExternalName().empty())
      return make<OrdinalOnlyChunk>(ctx, s->getOrdinal());
    return make<LookupChunk>(ctx, s->file->hintName);
  };
  auto define = [&](const Twine &name, Chunk *c) {
    StringRef saved = saver().save(name);
    if (Symbol *s = ctx.symtab.find(saved); s && isa<DefinedSynthetic>(s))
      replaceSymbol<DefinedSynthetic>(s, s->getName(), c);
    else
      ctx.symtab.addSynthetic(saved, c);
  };

  // Create .didat contents for each DLL.
  for (std::vector<DefinedImportData *> &syms : v) {
    // Create the delay import table header.
    dllNames.push_back(make<StringChunk>(syms[0]->getDLLName()));
    auto *dir = make<DelayDirectoryChunk>(dllNames.back());

    // A DLL has a gate where a word holds the DLL's own address of one of
    // its functions, code loads one from its cell, or an object names the
    // DLL's ensure stub. The gate imports the first function with such an
    // address, or the DLL's first, and holds that function's thunk.
    std::string dll = syms[0]->getDLLName().lower();
    DefinedImportData *gate = nullptr;
    Chunk *gateThunk = nullptr;
    if (ctx.symtab.delayResolve && !ctx.hybridSymtab &&
        !isArm64EC(ctx.config.machine)) {
      auto i = llvm::find_if(
          syms, [](DefinedImportData *s) { return s->file->observed; });
      if (hasWords.contains(dll) || namesDelayGate(ctx, dll) ||
          llvm::any_of(syms, [](DefinedImportData *s) {
            return s->file->observed && (s->file->uses & ImportFile::Loaded);
          }))
        gate = i != syms.end() ? *i : syms[0];
    }

    size_t base = addresses.size();
    ctx.forEachSymtab([&](SymbolTable &symtab) {
      if (symtab.isEC()) {
        if (ctx.config.machine == ARM64X) {
          // For hybrid images, emit null-terminated native import entries
          // followed by null-terminated EC entries. If a view is missing
          // imports for a given module, only terminators are emitted. Emit
          // ARM64X relocations to skip native entries in the EC view.
          ctx.dynamicRelocs->add(
              IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA, 0,
              Arm64XRelocVal(dir, offsetof(delay_import_directory_table_entry,
                                           DelayImportAddressTable)),
              (addresses.size() - base) * sizeof(uint64_t));
          ctx.dynamicRelocs->add(
              IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA, 0,
              Arm64XRelocVal(dir, offsetof(delay_import_directory_table_entry,
                                           DelayImportNameTable)),
              (addresses.size() - base) * sizeof(uint64_t));
          if (ctx.config.delayUnload)
            ctx.dynamicRelocs->add(
                IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA, 0,
                Arm64XRelocVal(dir, offsetof(delay_import_directory_table_entry,
                                             UnloadDelayImportTable)),
                (addresses.size() - base) * sizeof(uint64_t));
        } else {
          base = addresses.size();
        }
      }

      Chunk *tm = nullptr;

      for (DefinedImportData *s : syms) {
        // Process only the symbols belonging to the current symtab.
        if (symtab.isEC() != s->file->isEC())
          continue;

        if (!tm) {
          tm = newTailMergeChunk(symtab, dir);
          Chunk *pdataChunk = newTailMergePDataChunk(symtab, tm);
          if (pdataChunk)
            pdata.push_back(pdataChunk);
        }

        Chunk *t = newThunkChunk(s, tm);
        if (s == gate)
          gateThunk = t;
        auto *a = make<DelayAddressChunk>(ctx, t);
        addresses.push_back(a);
        if (ctx.config.delayUnload)
          unloads.push_back(make<DelayAddressChunk>(ctx, t));
        s->setLocation(a);
        thunks.push_back(t);
        StringRef extName = s->getExternalName();
        if (extName.empty()) {
          names.push_back(make<OrdinalOnlyChunk>(ctx, s->getOrdinal()));
        } else {
          auto *c = make<HintNameChunk>(extName, s->getOrdinal());
          names.push_back(make<LookupChunk>(ctx, c));
          hintNames.push_back(c);
          s->file->hintName = c;
        }
        // Add a synthetic symbol for this load thunk, using the
        // "__imp___load" prefix, in case this thunk needs to be added to the
        // list of valid call targets for Control Flow Guard.
        StringRef symName = saver().save(
            "__imp___load_" +
            (extName.empty() ? s->getName().substr(strlen("__imp_")) : extName));
        s->loadThunkSym =
            cast<DefinedSynthetic>(symtab.addSynthetic(symName, t));

        if (symtab.isEC()) {
          auto chunk = make<AuxImportChunk>(s->file);
          auxIat.push_back(chunk);
          s->file->impECSym->setLocation(chunk);

          chunk = make<AuxImportChunk>(s->file);
          auxIatCopy.push_back(chunk);
          s->file->auxImpCopySym->setLocation(chunk);
        } else if (ctx.config.machine == ARM64X) {
          // Fill the auxiliary IAT with null chunks for native imports.
          auxIat.push_back(make<NullChunk>(ctx));
          auxIatCopy.push_back(make<NullChunk>(ctx));
        }
      }

      if (tm) {
        thunks.push_back(tm);
        StringRef tmName =
            saver().save("__tailMerge_" + syms[0]->getDLLName().lower());
        symtab.addSynthetic(tmName, tm);
      }

      // Skip terminators on pure ARM64EC target if there are no native imports.
      if (!tm && !symtab.isEC() && ctx.config.machine != ARM64X)
        return;

      // Terminate with null values.
      addresses.push_back(make<NullChunk>(ctx, 8));
      names.push_back(make<NullChunk>(ctx, 8));
      if (ctx.config.delayUnload)
        unloads.push_back(make<NullChunk>(ctx, 8));
      if (ctx.symtab.isEC()) {
        auxIat.push_back(make<NullChunk>(ctx, 8));
        auxIatCopy.push_back(make<NullChunk>(ctx, 8));
      }
    });

    auto *mh = make<NullChunk>(8, 8);
    moduleHandles.push_back(mh);

    // Fill the delay import table header fields.
    dir->moduleHandle = mh;
    dir->addressTab = addresses[base];
    dir->nameTab = names[base];
    if (ctx.config.delayUnload)
      dir->unloadTab = unloads[base];
    dirs.push_back(dir);
    if (!gate)
      continue;

    // The gate is in the section of the DLL's table, and its descriptor is
    // among the DLLs' own, which precede every run's: the loader protects
    // the section of the first descriptor's table from the start.
    auto *list = make<DelayListChunk>();
    list->tables.push_back(
        {dir, addresses[base], 0, uint32_t(addresses.size() - base - 1)});
    auto *gateDir = make<DelayDirectoryChunk>(dllNames.back());
    gateDir->moduleHandle = mh;
    gateDir->addressTab = make<DelayAddressChunk>(ctx, gateThunk, true);
    gateDir->nameTab = newName(gate);
    for (std::vector<Chunk *> *tab : {&addresses, &names, &unloads}) {
      if (tab == &unloads && !ctx.config.delayUnload)
        continue;
      tab->push_back(tab == &addresses ? gateDir->addressTab
                     : tab == &names ? gateDir->nameTab
                                     : make<DelayAddressChunk>(ctx, gateThunk, true));
      tab->push_back(make<NullChunk>(ctx, 8));
    }
    gateDir->list = list;
    dirs.push_back(gateDir);
    list->gate = {gateDir, gateDir->addressTab, 0, 1};
    lists.push_back(list);
    gates[dll] = {dllNames.back(), mh, list};
    define("__ensure_" + dll, newEnsureChunk(list));
    define("__gate_" + dll, gateDir->addressTab);
    define("__gate_thunk_" + dll, gateThunk);
  }

  // A delay-bound word holds the function's load thunk until bound, as its
  // entry does, so that the address is one while the DLL is not loaded.
  for (const std::vector<ImportSlot> *w : {&runs, &words})
    for (const ImportSlot &s : *w)
      getScannedAt(*s.chunk, s.offset).sym = s.sym->loadThunkSym;

  // Each run is the address table of a descriptor of its own, whose name
  // table goes on to the zero word that ends the table for the loader: each
  // word between the run and that one gets an ordinal that no DLL exports,
  // so that the loader leaves it as it is.
  for (size_t i = 0, e = runs.size(); i != e;) {
    size_t first = i;
    Gated &g = gates.find(runs[i].sym->getDLLName().lower())->second;
    auto *runDir = make<DelayDirectoryChunk>(g.name);
    runDir->moduleHandle = g.handle;
    runDir->addressTab = runs[i].chunk;
    runDir->addressOffset = runs[i].offset;
    runDir->nameTab = newName(runs[i].sym);
    names.push_back(runDir->nameTab);
    while (++i != e && !runs[i].startsRun)
      names.push_back(newName(runs[i].sym));
    uint32_t pad;
    for (uint32_t n =
             getRunTail(runs[i - 1].chunk, runs[i - 1].offset + 8, pad);
         n; --n)
      names.push_back(make<OrdinalOnlyChunk>(ctx, 0xffff));
    names.push_back(make<NullChunk>(ctx, 8));
    dirs.push_back(runDir);
    g.list->tables.push_back({runDir, runDir->addressTab, runDir->addressOffset,
                              uint32_t(i - first)});
  }
  for (const ImportSlot &s : words)
    gates.find(s.sym->getDLLName().lower())->second.list->words.push_back(s);

  ctx.forEachSymtab([&](SymbolTable &symtab) {
    if (symtab.tailMergeUnwindInfoChunk)
      unwindinfo.push_back(symtab.tailMergeUnwindInfoChunk);
  });
  // Add null terminator.
  dirs.push_back(
      make<NullChunk>(sizeof(delay_import_directory_table_entry), 4));
}

namespace {
// The function table entries of the guards' leaves that sites call.
class DelayGuardPDataChunk : public NonSectionChunk {
public:
  DelayGuardPDataChunk(COFFLinkerContext &ctx) : ctx(ctx) { setAlignment(4); }
  size_t getSize() const override {
    return 12 * llvm::count_if(ctx.delayGuards, [](auto &g) {
             return toDelayGuard(g.second)->called;
           });
  }
  void writeTo(uint8_t *buf) const override {
    for (auto [key, leaf] : ctx.delayGuards)
      if (DelayGuardChunk *g = toDelayGuard(leaf); g->called) {
        write32le(buf, g->getRVA());
        write32le(buf + 4, g->getRVA() + g->getEnd());
        write32le(buf + 8, unwind[g->gates.size() - 1]
                                 [g->insn.empty() ? g->reg != 1 : 2]
                                     ->getRVA());
        buf += 12;
      }
  }
  // By the count of gates less one: where h is rax, where it is rcx, and
  // for a replay.
  SmallVector<std::array<Chunk *, 3>, 1> unwind;

private:
  COFFLinkerContext &ctx;
};
} // namespace

Chunk *DelayLoadContents::newTailMergeChunk(SymbolTable &symtab, Chunk *dir) {
  auto helper = cast_or_null<Defined>(symtab.delayLoadHelper);
  switch (symtab.machine) {
  case AMD64:
  case ARM64EC:
    return newStub(AMD64, true, tailMergeX64, {{54, dir}, {59, helper}});
  case I386:
    return make<TailMergeChunkX86>(ctx, dir, helper);
  case ARMNT:
    return make<TailMergeChunkARM>(ctx, dir, helper);
  case ARM64:
    return newStub(ARM64, true, tailMergeARM64,
                   {{48, dir}, {52, dir}, {56, helper}, {60, helper}});
  default:
    llvm_unreachable("unsupported machine type");
  }
}

// Returns the function table entry of a stub's code: its bounds and unwind
// information on x86-64; and on ARM64 its start and unwind information, or
// with none the packed form of the frame record that the code's first
// instruction allocates and that x29 points to, with the one epilogue at the
// end.
static Chunk *newPDataChunk(Chunk *code, Chunk *unwind) {
  static const uint8_t entry[12] = {};
  uint32_t size = code->getSize();
  if (code->getMachine() == AMD64)
    return newStub(AMD64, false, entry,
                   {{0, code}, {4, code, size}, {8, unwind}});
  if (unwind)
    return newStub(ARM64, false, ArrayRef(entry, 8), {{0, code}, {4, unwind}});
  // stp x29, x30, [sp, #-frame]!
  uint32_t stp = read32le(static_cast<StubChunk *>(code)->bytes.data());
  auto *packed = make<std::array<uint8_t, 8>>();
  write32le(packed->data() + 4, 1 | size / 4 << 2 | 3 << 21 |
                                    -SignExtend32<7>(stp >> 15) / 2 << 23);
  return newStub(ARM64, false, *packed, {{0, code}});
}

// Returns the unwind information of an x86-64 stub of length bytes from its
// version 1 form, by which an unwinder finds an epilogue in the code. An
// image of Windows Itanium or NT-POSIX takes version 2, as its compiled code
// does, so that no unwinder reads the code: epilogs are where the stub's
// epilogues start, at the first pop or else at the terminator, and size
// spans each up to the terminator's first byte.
static ArrayRef<uint8_t> x64Unwind(COFFLinkerContext &ctx, ArrayRef<uint8_t> v1,
                                   uint8_t size, uint8_t length,
                                   std::initializer_list<uint8_t> epilogs) {
  if (!ctx.hasForkObject)
    return v1;
  // The first code states the size, and whether the last epilogue ends the
  // stub, which it then names; a code for each other states its distance
  // from the end, the last listed last. The codes fill an even number of
  // slots, ahead of the prolog's.
  ArrayRef<uint8_t> e = epilogs;
  bool atEnd = e.back() + size == length;
  auto &info = *make<SmallVector<uint8_t, 24>>(v1.take_front(4));
  info.append({size, uint8_t(atEnd << 4 | Win64EH::UOP_Epilog)});
  for (uint8_t start : e.drop_back(atEnd))
    info.append({uint8_t(length - start), Win64EH::UOP_Epilog});
  if (info.size() % 4)
    info.append({0, Win64EH::UOP_Epilog});
  info.append(v1.begin() + 4, v1.begin() + 4 + 2 * v1[2]);
  info[0] = (v1[0] & ~7) | 2;
  info[2] = info.size() / 2 - 2;
  info.resize(alignTo(info.size(), 4));
  return info;
}

Chunk *DelayLoadContents::newTailMergePDataChunk(SymbolTable &symtab,
                                                 Chunk *tm) {
  switch (symtab.machine) {
  case AMD64:
  case ARM64EC:
    if (!symtab.tailMergeUnwindInfoChunk)
      symtab.tailMergeUnwindInfoChunk =
          newStub(AMD64, false,
                  x64Unwind(ctx, tailMergeUnwindInfoX64, 1,
                            sizeof(tailMergeX64), {sizeof(tailMergeX64) - 2}));
    return newPDataChunk(tm, symtab.tailMergeUnwindInfoChunk);
  case ARM64:
    return newPDataChunk(tm, nullptr);
  default:
    return nullptr; // Just don't generate unwind info.
  }
}

// The bytes and the instructions of an ARM64 unwind code, as
// RtlpUnwindFunctionFull counts them: a custom code is no instruction.
static unsigned getUnwindCodeSize(uint8_t c) {
  static const uint8_t wide[32] = {4, 1, 2, 1, 1, 1, 1, 3, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 2, 3, 4, 5, 1, 1, 1, 1};
  return c < 0xc0 ? 1 : c < 0xe0 ? 2 : wide[c - 0xe0];
}
static bool isUnwindEnd(uint8_t c) { return (c & 0xfe) == 0xe4; }

// The unwind information that ntdll makes of packed ARM64 unwind data w
// before it unwinds (RtlpExpandCompactToFull): one header word, then the
// codes of the canonical prologue, those of the last instruction first, and
// one allocation of any size. With flag 1 one epilogue at the end shares
// them past the set_fp, or with the home area saved is a copy of them
// without its nops; with flag 2 an end_c leaves neither.
static std::vector<uint8_t> expandPackedUnwind(uint32_t w) {
  unsigned flag = w & 3, regF = w >> 13 & 7, regI = w >> 16 & 15,
           cr = w >> 21 & 3, ints = regI + (cr == 1), fps = regF ? regF + 1 : 0;
  // The saved registers and the locals, in words.
  unsigned saved = ints + fps ? ints + fps + 8 * (w >> 20 & 1) : 0;
  saved = (saved + 1) & ~1u;
  int local = 2 * (w >> 23) - saved;
  // The codes, each's bytes last first, as the prologue runs.
  std::vector<uint8_t> c = {0xe4};
  auto put = [&](uint32_t v, unsigned n) {
    for (unsigned i = 0; i != n; ++i)
      c.push_back(v >> 8 * i);
  };
  auto alloc = [&](uint32_t bytes) {
    if (bytes >= 0x4000)
      put(0xe0000000 | bytes >> 4, 4);
    else if (bytes >= 0x200)
      put(0xc000 | bytes >> 4, 2);
    else
      put(bytes >> 4 & 0x1f, 1);
  };
  bool started = false, fp = false;
  unsigned off = 0, i = 0;
  if (cr == 2)
    put(0xfc, 1);
  if (ints) {
    if (regI == 1 && cr == 1)
      alloc(saved * 8), started = true;
    for (; i != (regI & ~1u); i += 2, off += 2, started = true)
      put((started ? 0xc800 | off : 0xcc00 | (saved - 1)) | i << 6, 2);
    if (regI & 1 && cr == 1)
      put(0xd600 | (i / 2 & 7) << 6 | off, 2), off += 2;
    else if (regI & 1)
      put(started ? 0xd000 | i << 6 | off : 0xd400 | (saved - 1), 2), ++off;
    else if (cr == 1)
      put(started ? 0xd2c0 | off : 0xd560 | (saved - 1), 2), ++off;
    started = true;
  }
  for (i = 0; i != (fps & ~1u); i += 2, off += 2, started = true)
    put((started ? 0xd800 | off : 0xda00 | (saved - 1)) | i << 6, 2);
  if (fps & 1)
    put(started ? 0xdc00 | i << 6 | off : 0xde00 | i << 5 | (saved - 1), 2);
  int home = -1;
  if (w >> 20 & 1 && started)
    home = c.size(), put(0xe3e3e3e3, 4);
  if (local > 0 && cr >= 2) {
    if (local > 0x40)
      alloc(local * 8), put(0x40, 1);
    else
      put(0x80 | (local - 1), 1);
    put(0xe1, 1), fp = true;
  } else if (local > 0) {
    alloc(local * 8);
  }
  if (flag == 2)
    put(0xe5, 1);
  std::reverse(c.begin(), c.end());
  uint32_t n = c.size(),
           header = (w >> 2 & 0x7ff) | (flag == 2 ? 0 : 1 << 21 | fp << 22);
  if (home >= 0 && flag != 2) {
    for (uint32_t j = 0; j != n; ++j)
      if (j + home + 4 < n || j + home + 1 > n)
        c.push_back(c[j]);
    header += n << 22;
  }
  header |= (c.size() + 3) / 4 << 27;
  std::vector<uint8_t> x(4);
  write32le(x.data(), header);
  llvm::append_range(x, c);
  x.resize(alignTo(x.size(), 4));
  return x;
}

std::vector<uint8_t> getBodyRecord(uint32_t words, StringRef codes) {
  std::vector<uint8_t> v(8, 0);
  v.push_back(0xe5);
  llvm::append_range(v, codes);
  v.push_back(0xe4);
  v.resize(alignTo(v.size(), 4), 0xe3);
  uint32_t n = v.size() / 4 - 2;
  if (n < 32)
    v.erase(v.begin(), v.begin() + 4);
  write32le(v.data(), words | (n < 32 ? n << 27 : 0));
  if (n >= 32)
    write32le(v.data() + 4, n << 16);
  return v;
}

// The codes of the ARM64 function that covers off in sc that ntdll runs for
// a PC there (RtlpUnwindFunctionFull): of the function table entry that
// starts last at or before off, the prologue's less those of the
// instructions not yet run, an epilogue's less those it has run, else all,
// to the end; none where no entry covers off, which is a leaf. edge says
// that off is in the prologue or an epilogue.
std::optional<std::string> getUnwindCodesAt(const SectionChunk &sc,
                                            uint32_t off, bool &edge) {
  const SectionChunk *xdata = nullptr;
  uint32_t begin = 0, info = 0;
  bool found = false;
  edge = false;
  // Where the link resized sc, its symbols have their output offsets.
  ScannedSection *s = sc.resizable ? sc.getScanned() : nullptr;
  if (s && !(s->aux && s->aux->resized))
    s = nullptr;
  for (Chunk *c : sc.file->getChunks()) {
    auto *pdata = dyn_cast_or_null<SectionChunk>(c);
    if (!pdata || !pdata->getSectionName().starts_with(".pdata"))
      continue;
    ArrayRef<uint8_t> d = pdata->getContents();
    for (const coff_relocation &rel : pdata->getRelocs()) {
      auto *f = dyn_cast_or_null<DefinedRegular>(pdata->getRelocTarget(rel));
      uint32_t at = rel.VirtualAddress, start;
      if (at % 8 || at + 8 > d.size() || !f || f->getChunk() != &sc ||
          (start = (s ? getInputOffset(*s, f) : f->getValue()) +
                   read32le(&d[at])) > off ||
          (found && start < begin))
        continue;
      found = true, begin = start, xdata = nullptr;
      info = read32le(&d[at + 4]);
      for (const coff_relocation &u : pdata->getRelocs())
        if (u.VirtualAddress == at + 4)
          if (auto *x = dyn_cast_or_null<DefinedRegular>(
                  pdata->getRelocTarget(u)))
            xdata = x->getChunk(), info += x->getValue();
    }
  }
  if (!found)
    return std::string();
  std::vector<uint8_t> packed;
  ArrayRef<uint8_t> x;
  if (xdata && info + 8 <= xdata->getSize())
    x = xdata->getContents().drop_front(info);
  else if (!xdata && (info & 3) && (info & 3) != 3)
    x = packed = expandPackedUnwind(info);
  else
    return std::nullopt;
  ARM::WinEH::ExceptionDataRecord h(
      reinterpret_cast<const support::ulittle32_t *>(x.data()), true);
  uint32_t k = (off - begin) / 4, scopes = h.E() ? 0 : h.EpilogueCount();
  if (k >= h.FunctionLengthInBytesAArch64() / 4)
    return std::string();
  ArrayRef<uint8_t> codes =
      x.slice(4 * (ARM::WinEH::HeaderWords(h) + scopes), 4 * h.CodeWords());
  // The instructions of the codes from i to their end, and the code n codes
  // after i, or before an end.
  auto count = [&](size_t i) {
    unsigned n = 0;
    for (; i < codes.size() && !isUnwindEnd(codes[i]);
         i += getUnwindCodeSize(codes[i]))
      n += codes[i] < 0xe8 || codes[i] >= 0xf8;
    return n;
  };
  auto skip = [&](size_t i, unsigned n) {
    for (; n && i < codes.size() && !isUnwindEnd(codes[i]); --n)
      i += getUnwindCodeSize(codes[i]);
    return i;
  };
  size_t start = 0;
  if (unsigned prologue = count(0); k < prologue) {
    start = skip(0, prologue - k), edge = true;
  } else if (h.E()) {
    unsigned epilogue = h.FunctionLengthInBytesAArch64() / 4 -
                        count(h.EpilogueCount()) - 1;
    if (k >= epilogue)
      start = skip(h.EpilogueCount(), k - epilogue), edge = true;
  } else {
    for (uint32_t i = 0; i != scopes; ++i) {
      uint32_t w = read32le(&x[4 * (ARM::WinEH::HeaderWords(h) + i)]);
      uint32_t from = w & 0x3ffff, index = w >> 22;
      if (k < from)
        break;
      if (k < from + count(index) + 1) {
        start = skip(index, k - from), edge = true;
        break;
      }
    }
  }
  size_t end = start;
  while (end < codes.size() && codes[end] != 0xe4)
    end += getUnwindCodeSize(codes[end]);
  return std::string(codes.begin() + start,
                     codes.begin() + std::min(end, codes.size()));
}

Defined *addDelayGuardA64(COFFLinkerContext &ctx, const DelayGuardSite &s) {
  bool edge;
  std::optional<std::string> codes = getUnwindCodesAt(
      *s.chunk, s.chunk->getRelocs()[s.index].VirtualAddress, edge);
  if (!codes)
    return nullptr;
  if (edge)
    s.chunk->getScanned()->relocs[s.index].expr = R_GUARD_CALL;
  auto *g =
      make<DelayGuardA64Chunk>(s.chunk, s.index, getGates(ctx, s.value),
                               s.value, s.reg, s.chunk->getSize() > 0x80000);
  g->codes = std::move(*codes);
  ctx.delayGuardsA64.push_back(g);
  return make<DefinedSynthetic>("__delay_guard", g);
}

Chunk *DelayLoadContents::newEnsureChunk(Chunk *list) {
  auto *resolve = dyn_cast<Defined>(ctx.symtab.delayResolve);
  bool x64 = ctx.config.machine == AMD64;
  if (!ensureUnwind) {
    if (!resolve)
      Err(ctx) << "the address of a delay-loaded function is its DLL's own, "
                  "which needs __llvm_delay_resolve; link the builtins "
                  "library, or keep the thunk's with /delay:nocanonical:<dll>";
    ensureUnwind =
        newStub(ctx.config.machine, false,
                x64 ? x64Unwind(ctx, ensureUnwindInfoX64, 2, sizeof(ensureX64),
                                {sizeof(ensureX64) - 2})
                    : ArrayRef(ensureUnwindInfoARM64));
    unwindinfo.push_back(ensureUnwind);
    // The guards' leaves come with the first ensure stub.
    if (x64 && !ctx.delayGuards.empty()) {
      auto *entries = make<DelayGuardPDataChunk>(ctx);
      // The epilogue is before a call of an ensure stub for each gate.
      size_t gates = 0;
      for (auto [key, leaf] : ctx.delayGuards)
        gates = std::max(gates, toDelayGuard(leaf)->gates.size());
      entries->unwind.resize(gates);
      for (size_t n = 1; n <= gates; ++n)
        for (int k : {0, 1, 2})
          unwindinfo.push_back(
              entries->unwind[n - 1][k] = newStub(
                  AMD64, false,
                  x64Unwind(ctx, guardUnwindInfoX64[k], 2 + 2 * (k == 2),
                            2 + 2 * (k == 2) + 7 * n, {0})));
      for (auto [key, leaf] : ctx.delayGuards)
        thunks.push_back(toDelayGuard(leaf));
      pdata.push_back(entries);
    }
    // An ARM64 guard's head of each block, the gate's test with the site's
    // address and the stp, and its tail, the site's adrp and the b back,
    // change only the site's register: their codes are those ntdll runs for
    // the site, after an end_c that makes every point the body. The call and
    // the restore after it report the site, with x30 from the guard's +8
    // then +0, and the 16 bytes: the dispatcher unwinds a call at its bl.
    // Past 31 words of codes the header has its second word.
    if (!x64 && !ctx.delayGuardsA64.empty()) {
      std::map<std::string, Chunk *> shared;
      auto record = [&](uint32_t words, StringRef codes) {
        Chunk *&c = shared[(Twine(char(words)) + codes).str()];
        if (!c)
          unwindinfo.push_back(c = newStub(ARM64, false,
                                           *make<std::vector<uint8_t>>(
                                               getBodyRecord(words, codes))));
        return c;
      };
      static const uint8_t entry[8] = {};
      for (Chunk *c : ctx.delayGuardsA64) {
        auto *g = static_cast<DelayGuardA64Chunk *>(c);
        auto add = [&](uint32_t at, Chunk *unwind) {
          pdata.push_back(
              newStub(ARM64, false, entry, {{0, g, at}, {4, unwind}}));
        };
        for (size_t i = 0, e = g->gates.size(); i != e; ++i) {
          add(i * g->getBlock(), record(g->getHead(), g->codes));
          add(i * g->getBlock() + 4 * g->getHead(),
              record(g->getBlock() / 4 - g->getHead(),
                     "\xd2\xc1\xec\xd2\xc0\x01"));
        }
        add(g->gates.size() * g->getBlock(), record(2, g->codes));
      }
    }
  }
  Chunk *code =
      x64 ? newStub(AMD64, true, ensureX64, {{0x58, list}, {0x64, resolve}})
          : newStub(
                ARM64, true, ensureARM64,
                {{0x7c, list}, {0x80, list}, {0x84, resolve}, {0x88, resolve}});
  thunks.push_back(code);
  pdata.push_back(newPDataChunk(code, ensureUnwind));
  return code;
}

Chunk *DelayLoadContents::newThunkChunk(DefinedImportData *s,
                                        Chunk *tailMerge) {
  switch (s->file->getMachineType()) {
  case AMD64:
  case ARM64EC:
    return newStub(AMD64, true, thunkX64, {{3, s}, {8, tailMerge}});
  case I386:
    return make<ThunkChunkX86>(ctx, s, tailMerge);
  case ARMNT:
    return make<ThunkChunkARM>(ctx, s, tailMerge);
  case ARM64: {
    StubChunk *far =
        newStub(ARM64, true, thunkARM64, {{0, s}, {4, s}, {8, tailMerge}});
    if (!ctx.hasForkObject)
      return far;
    StubChunk *c =
        newStub(ARM64, true, thunkNearARM64, {{0, s}, {4, tailMerge}});
    c->far = far;
    return c;
  }
  default:
    llvm_unreachable("unsupported machine type");
  }
}

// What the runtime's fill walks, compiler-rt's FillList: a header of 32
// bytes, with the number of ranges, cells and words, how many cells are name
// words, the sealed section, and the entry of a DLL's offer table that names
// the DLL, where it has a vague cell or a name word; then the entries, and the names that they point at, with
// every address relative to the image base.
namespace {
class FillListChunk : public NonSectionChunk {
public:
  // The names that the imports' entries point at follow them: the DLLs of
  // the weak imports, each once, in UTF-16, and then the imports'.
  FillListChunk(COFFLinkerContext &ctx, OutputSection *sealed)
      : ctx(ctx), sealed(sealed) {
    setAlignment(8);
    // A DLL that searches the process's images for an instance first asks
    // whether the executable's link saw it, by its name, which the executable's
    // offer table then names.
    if (ctx.config.dll && ctx.symtab.offerTable &&
        llvm::any_of(ctx.fill.words, isVagueCell)) {
      self = saver().save(StringRef(ctx.driver.getImportName(false)).lower());
      ctx.symtab.offerTable->entries.push_back(
          {self, nullptr, LinkImageEntityImage, 0,
           ctx.symtab.offerTable->identity});
    }
    DenseMap<StringRef, uint32_t> dlls;
    SmallVector<DefinedImportData *, 0> imports;
    for (const FillWord &w : ctx.fill.words)
      if (w.kind == FillWord::Import)
        imports.push_back(cast<DefinedImportData>(w.source));
    for (DefinedImportData *imp : imports) {
      if (imp->file->binding != ImportFile::Weak)
        continue;
      auto [it, added] = dlls.try_emplace(imp->getDLLName(), names.size());
      SmallVector<UTF16, 0> wide;
      if (!added)
        continue;
      if (!convertUTF8ToUTF16String(it->first, wide) || wide.empty())
        Err(ctx) << "the name of " << it->first << " is not UTF-8";
      for (UTF16 c : wide)
        names.append({uint8_t(c), uint8_t(c >> 8)});
      names.append({0, 0});
    }
    for (DefinedImportData *imp : imports) {
      ImportFile::Binding b = imp->file->binding;
      at[imp] = {uint32_t(names.size()), b == ImportFile::Weak
                                             ? dlls[imp->getDLLName()]
                                             : b == ImportFile::HostWeak};
      llvm::append_range(names, imp->getExternalName());
      names.push_back(0);
    }
  }
  size_t getSize() const override {
    return 32 + 8 * ctx.fill.ranges.size() + 16 * ctx.fill.words.size() +
           names.size();
  }

  void writeTo(uint8_t *buf) const override {
    memset(buf, 0, getSize());
    write32le(buf, ctx.fill.ranges.size());
    write32le(buf + 4, llvm::count_if(ctx.fill.words, [](const FillWord &w) {
                return w.kind == FillWord::Cell;
              }));
    write32le(buf + 8, ctx.fill.words.size());
    write32le(buf + 12, llvm::count_if(ctx.fill.words, [](const FillWord &w) {
                return w.kind == FillWord::Cell && w.addend == NameCell;
              }));
    // An image whose fill writes no read-only memory has no .sealed, which
    // is then empty at zero.
    write32le(buf + 16, sealed->getRVA());
    write32le(buf + 20, sealed->getVirtualSize());
    if (!self.empty())
      write32le(buf + 24, ctx.symtab.offerTable->getImageEntry(self));
    memcpy(writeWords(writeRanges(buf + 32)), names.data(), names.size());
  }

private:
  // A range goes from the first to the last of its section's words.
  uint8_t *writeRanges(uint8_t *buf) const {
    for (OutputSection *sec : ctx.fill.ranges) {
      uint32_t begin = UINT32_MAX, end = 0;
      for (const FillWord &w : ctx.fill.words)
        if (ctx.getOutputSection(w.chunk) == sec) {
          begin = std::min<uint32_t>(begin, w.chunk->getRVA() + w.offset);
          end = std::max<uint32_t>(end, w.chunk->getRVA() + w.offset + 8);
        }
      write32le(buf, begin);
      write32le(buf + 4, end - begin);
      buf += 8;
    }
    return buf;
  }

  // The words, a kind at a time, the greater first. First the cells, the vague ones first and then the name words: each
  // with its kind, its CellUse and slot above that, and the address of the
  // image's own default where the cell is the image's, or for a vague cell or
  // a name word the top 24 bits of its entity's key in place of the slot, and
  // its entry in the offer table and, where the cell is the image's, its
  // instance above that, or for a name word the name that it holds. Then the
  // imports
  // that the fill binds by name: each with its pointer, its kind, its name and
  // where it is: 0 or, for a weak import, 1 for the executable that loads the
  // image, or the name of its DLL. Then the others in the order of their
  // addresses: each with the entry of the address table that it is computed
  // from, which is 8-aligned, the kind in its low three bits, and the offset
  // that the object has in it, or for a TlsOffset word the record's word.
  uint8_t *writeWords(uint8_t *buf) const {
    uint32_t pool = getRVA() + getSize() - names.size();
    std::vector<FillWord> words = ctx.fill.words;
    // A TlsOffset word follows the Init words, whose values it may read.
    auto order = [](const FillWord &w) {
      return std::tuple(w.kind == FillWord::TlsOffset ? 1 - 2 * FillWord::Init
                                                      : -2 * w.kind,
                        isVagueCell(w) ? w.addend - VagueCell : 2,
                        w.chunk->getRVA() + w.offset);
    };
    llvm::sort(words, [&](const FillWord &a, const FillWord &b) {
      return order(a) < order(b);
    });
    for (const FillWord &w : words) {
      write32le(buf, w.chunk->getRVA() + w.offset);
      if (w.kind == FillWord::Import) {
        auto *imp = cast<DefinedImportData>(w.source);
        std::array<uint32_t, 2> name = at.lookup(imp);
        write32le(buf + 4, FillWord::Import);
        write32le(buf + 8, pool + name[0]);
        write32le(buf + 12, imp->file->binding == ImportFile::Weak
                                ? pool + name[1]
                                : name[1]);
        buf += 16;
        continue;
      }
      if (w.kind == FillWord::Cell) {
        auto *own = dyn_cast_or_null<DefinedLocalImport>(w.source);
        uint64_t source = FillWord::Cell | w.addend << 3;
        uint64_t addend = own ? own->getTarget()->getRVA() : 0;
        if (isVagueCell(w)) {
          auto [entry, key] = ctx.symtab.offerTable->getEntry(w.source);
          source |= key >> 40 << 8;
          // A name word's object's name is the name that it holds.
          if (w.addend == NameCell) {
            std::string name =
                ("_ZTS" + w.source->getName().drop_front(4)).str();
            addend = cast<Defined>(ctx.symtab.find(name))->getRVA();
          }
          addend = entry | addend << 32;
        }
        write32le(buf + 4, source);
        write64le(buf + 8, addend);
        buf += 16;
        continue;
      }
      // The fill opens no other read-only memory than what the list names.
      OutputSection *sec = ctx.getOutputSection(w.chunk);
      assert((sec->header.Characteristics & IMAGE_SCN_MEM_WRITE) ||
             sec == sealed || llvm::is_contained(ctx.fill.ranges, sec));
      // The ntposix runtime exchanges such a word in each thread's copy.
      if (sec->name == ".tls" && !ctx.tlsRuntime().empty() &&
          (w.chunk->getRVA() + w.offset - sec->getRVA()) % 8)
        reportFillWord(ctx, w) << ", at an offset from the start of the "
                                  "thread-local template that is not a "
                                  "multiple of 8 bytes";
      uint32_t source = w.source->getRVA();
      if (source % 8)
        reportFillWord(ctx, w) << ", from " << w.source->getName()
                               << ", which is not aligned to 8 bytes";
      write32le(buf + 4, source | w.kind);
      write64le(buf + 8, w.addend);
      buf += 16;
    }
    return buf;
  }

  COFFLinkerContext &ctx;
  OutputSection *sealed;
  StringRef self;
  SmallVector<uint8_t, 0> names;
  // The offsets in names of each import's name and its DLL's, or else 0 or 1.
  DenseMap<const DefinedImportData *, std::array<uint32_t, 2>> at;
};
} // namespace

void createFill(COFFLinkerContext &ctx, OutputSection *rdata,
                OutputSection *sealed) {
  Chunk *list = make<FillListChunk>(ctx, sealed);
  rdata->addChunk(list);
  Symbol *name = ctx.symtab.find("__llvm_fill_list");
  replaceSymbol<DefinedSynthetic>(name, name->getName(), list);
}

void createEdataChunks(SymbolTable &symtab, std::vector<Chunk *> &chunks) {
  unsigned baseOrdinal = 1 << 16, maxOrdinal = 0;
  for (Export &e : symtab.exports) {
    baseOrdinal = std::min(baseOrdinal, (unsigned)e.ordinal);
    maxOrdinal = std::max(maxOrdinal, (unsigned)e.ordinal);
  }
  // Ordinals must start at 1 as suggested in:
  // https://learn.microsoft.com/en-us/cpp/build/reference/export-exports-a-function?view=msvc-170
  assert(baseOrdinal >= 1);

  auto *dllName =
      make<StringChunk>(sys::path::filename(symtab.ctx.config.outputFile));
  auto *addressTab = make<AddressTableChunk>(symtab, baseOrdinal, maxOrdinal);
  std::vector<Chunk *> names;
  for (Export &e : symtab.exports)
    if (!e.noname)
      names.push_back(e.nameChunk = make<StringChunk>(e.exportName));

  std::vector<Chunk *> forwards;
  for (Export &e : symtab.exports) {
    if (e.forwardTo.empty())
      continue;
    e.forwardChunk = make<StringChunk>(e.forwardTo);
    forwards.push_back(e.forwardChunk);
  }

  auto *nameTab = make<NamePointersChunk>(names);
  auto *ordinalTab =
      make<ExportOrdinalChunk>(symtab, baseOrdinal, names.size());
  auto *dir =
      make<ExportDirectoryChunk>(baseOrdinal, maxOrdinal, names.size(), dllName,
                                 addressTab, nameTab, ordinalTab);
  chunks.push_back(dir);
  chunks.push_back(dllName);
  chunks.push_back(addressTab);
  chunks.push_back(nameTab);
  chunks.push_back(ordinalTab);
  chunks.insert(chunks.end(), names.begin(), names.end());
  chunks.insert(chunks.end(), forwards.begin(), forwards.end());
}

} // namespace lld::coff
