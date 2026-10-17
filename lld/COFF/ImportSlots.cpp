//===- ImportSlots.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ImportSlots.h"
#include "COFFLinkerContext.h"
#include "DLL.h"
#include "InputFiles.h"
#include "KCFI.h"
#include "LocalImports.h"
#include "OverrideTable.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;
using namespace llvm::support::endian;

namespace lld::coff {
namespace {
// Under -import-slots, the image's residual fill: an initializer, which the C
// initializer table runs before any other, that writes each residual word
// from its import's entry. When read-only words are among them, they are in a
// writable section of their own, which the function then makes read-only with
// NtProtectVirtualMemory; that call gives it a frame, which ResidualFillUnwind
// describes. It returns that call's status, so that a failed seal fails the
// image's start-up, as any nonzero return from the table does; without the
// call it returns 0.
class ResidualFillChunk : public NonSectionCodeChunk {
public:
  ResidualFillChunk(COFFLinkerContext &ctx, std::vector<ResidualWord> words,
                    std::vector<RunTimeCell> cells, Chunk *sealed,
                    DefinedImportData *protect, DefinedImportData *getProc,
                    DefinedImportData *getDll);
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override;
  // The size of the prologue, which sets up the frame, if there is one.
  size_t getPrologueSize() const;

private:
  size_t getWordSize(const ResidualWord &w) const;
  size_t getCellSize(const RunTimeCell &c) const;

  std::vector<ResidualWord> words;
  std::vector<RunTimeCell> cells;
  // A chunk of the section the function seals, or null.
  Chunk *sealed;
  // NtProtectVirtualMemory's import, when it seals.
  DefinedImportData *protect;
  // LdrGetProcedureAddressForCaller's and LdrGetDllHandleEx's imports, when it
  // binds cells.
  DefinedImportData *getProc;
  DefinedImportData *getDll;
  COFFLinkerContext &ctx;
};

// The cell of an import bound at run time: a word the fill writes, null until
// it does.
class RunTimeCellChunk : public NonSectionChunk {
public:
  explicit RunTimeCellChunk(COFFLinkerContext &ctx)
      : size(ctx.config.wordsize) {
    setAlignment(size);
  }
  size_t getSize() const override { return size; }
  void writeTo(uint8_t *buf) const override { memset(buf, 0, size); }

private:
  size_t size;
};

// A NUL-terminated UTF-16 string, the form ntdll takes a module's name in.
class WideStringChunk : public NonSectionChunk {
public:
  explicit WideStringChunk(StringRef s) {
    convertUTF8ToUTF16String(s, str);
    setAlignment(2);
  }
  size_t getSize() const override { return 2 * (str.size() + 1); }
  void writeTo(uint8_t *buf) const override {
    for (size_t i = 0; i != str.size(); ++i)
      write16le(buf + 2 * i, str[i]);
    write16le(buf + 2 * str.size(), 0);
  }

private:
  SmallVector<UTF16, 0> str;
};

// The unwind information of a fill function with a frame, which its first
// instruction sets up and its last two take down: the residual fill when it
// seals, the fill of overridable-default cells when it writes read-only ones,
// or a DLL's run of the executable's thread-local slot copy on AArch64.
class ResidualFillUnwindChunk : public NonSectionChunk {
public:
  ResidualFillUnwindChunk(COFFLinkerContext &ctx, Chunk *fill)
      : fill(fill), ctx(ctx) {
    setAlignment(4);
  }
  size_t getSize() const override { return 8; }
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  Chunk *fill;
  COFFLinkerContext &ctx;
};

// The exception table entry of a fill function with a frame.
class ResidualFillPdataChunk : public NonSectionChunk {
public:
  ResidualFillPdataChunk(COFFLinkerContext &ctx, Chunk *fill,
                         ResidualFillUnwindChunk *unwind)
      : fill(fill), unwind(unwind), ctx(ctx) {
    setAlignment(4);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
           llvm::COFF::IMAGE_SCN_MEM_READ;
  }

private:
  Chunk *fill;
  ResidualFillUnwindChunk *unwind;
  COFFLinkerContext &ctx;
};
ResidualFillChunk::ResidualFillChunk(COFFLinkerContext &ctx,
                                     std::vector<ResidualWord> words,
                                     std::vector<RunTimeCell> cells,
                                     Chunk *sealed, DefinedImportData *protect,
                                     DefinedImportData *getProc,
                                     DefinedImportData *getDll)
    : words(std::move(words)), cells(std::move(cells)), sealed(sealed),
      protect(protect), getProc(getProc), getDll(getDll), ctx(ctx) {
  // Control Flow Guard checks the initializer table's call to it.
  setAlignment(16);
}

MachineTypes ResidualFillChunk::getMachine() const {
  return ctx.config.machine;
}

size_t ResidualFillChunk::getPrologueSize() const {
  if (!protect)
    return 0;
  return ctx.config.machine == ARM64 ? 8 : 4;
}

// The instructions that add an addend to the import's address.
size_t getArm64AddendSize(int64_t addend) {
  if (addend > -4096 && addend < 4096)
    return 4;
  size_t n = 0;
  for (int shift = 0; shift != 64; shift += 16)
    n += (uint64_t(addend) >> shift & 0xFFFF) != 0;
  return 4 * std::max<size_t>(n, 1) + 4;
}

size_t ResidualFillChunk::getWordSize(const ResidualWord &w) const {
  if (ctx.config.machine == ARM64)
    return 8 + getArm64AddendSize(w.addend) + 12;
  return 7 + (isInt<32>(w.addend) ? 6 : 13) + 7;
}

// A cell is bound in the executable for a host import and in its DLL, found
// by name, for a weak one; a host import that is not weak fails the fill when
// the executable lacks it.
size_t ResidualFillChunk::getCellSize(const RunTimeCell &c) const {
  bool arm64 = ctx.config.machine == ARM64;
  size_t lookup = arm64 ? 64 : 61;
  if (c.dllName)
    return lookup + (arm64 ? 64 : 60);
  bool weak = c.imp->file->isWeakImport;
  return lookup + (arm64 ? 8 : 13) + (weak ? 0 : (arm64 ? 4 : 8));
}

size_t ResidualFillChunk::getSize() const {
  size_t size = getPrologueSize();
  for (const RunTimeCell &c : cells)
    size += getCellSize(c);
  for (const ResidualWord &w : words)
    size += getWordSize(w);
  if (ctx.config.machine == ARM64)
    return size + (protect ? 56 + 8 : 4) + 4;
  return size + (protect ? 60 + 4 : 2) + 1;
}

void ResidualFillChunk::writeTo(uint8_t *buf) const {
  const uint32_t pageReadOnly = 2;
  uint64_t sealStart = 0, sealSize = 0;
  if (protect) {
    OutputSection *os = ctx.getOutputSection(sealed);
    sealStart = os->getRVA();
    sealSize = os->getVirtualSize();
  }
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    // adrp xN, target; ldr xN, [xN, :lo12:target]
    auto load = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0xF9400000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Ldr(buf + off + 4, target & 0xfff);
      off += 8;
    };
    // adrp xN, target; add xN, xN, :lo12:target
    auto address = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    if (protect) {
      put(0xA9BD7BFD); // stp x29, x30, [sp, #-48]!
      put(0x910003FD); // mov x29, sp
    }
    // The epilogue, which a failed lookup of a host import branches to with
    // its status in w0.
    uint32_t end = getSize() - 12;
    // movz w16, #lo; movk w16, #hi, lsl #16
    auto movW16 = [&](uint32_t v) {
      put(0x52800010 | (v & 0xFFFF) << 5);
      put(0x72A00010 | (v >> 16) << 5);
    };
    // LdrGetProcedureAddressForCaller(x0, &name, 0, &cell, 1, fill), the
    // name an ANSI_STRING at sp + 16. Flag 1 binds without initializing the
    // module, as the loader's snap does.
    auto lookup = [&](const RunTimeCell &c) {
      uint32_t len = c.name->getSize() - 1;
      address(16, c.name->getRVA());
      put(0xF9000FF0); // str x16, [sp, #24]
      movW16(len | (len + 1) << 16);
      put(0xB90013F0); // str w16, [sp, #16]
      put(0x910043E1); // add x1, sp, #16
      put(0x52800002); // mov w2, #0
      address(3, c.cell->getRVA());
      put(0x52800024); // mov w4, #1
      address(5, rva);
      load(16, getProc->getRVA());
      put(0xD63F0200); // blr x16
    };
    for (const RunTimeCell &c : cells) {
      if (c.dllName) {
        // LdrGetDllHandleEx(1, null, null, &dllName, sp + 32), the name a
        // UNICODE_STRING at sp + 16, leaving the cell null when the DLL is
        // not loaded.
        uint32_t len = c.dllName->getSize() - 2;
        address(16, c.dllName->getRVA());
        put(0xF9000FF0); // str x16, [sp, #24]
        movW16(len | (len + 2) << 16);
        put(0xB90013F0); // str w16, [sp, #16]
        put(0x52800020); // mov w0, #1
        put(0xAA1F03E1); // mov x1, xzr
        put(0xAA1F03E2); // mov x2, xzr
        put(0x910043E3); // add x3, sp, #16
        put(0x910083E4); // add x4, sp, #32
        load(16, getDll->getRVA());
        put(0xD63F0200);           // blr x16
        put(0x37F80000 | 18 << 5); // tbnz w0, #31, past the lookup
        put(0xF94013E0);           // ldr x0, [sp, #32]
        lookup(c);
        continue;
      }
      put(0xF9403240); // ldr x0, [x18, #0x60]: the PEB
      put(0xF9400800); // ldr x0, [x0, #0x10]: the executable's base
      lookup(c);
      if (!c.imp->file->isWeakImport)
        put(0x37F80000 | ((end - off) / 4 & 0x3FFF) << 5); // tbnz w0, #31, end
    }
    for (const ResidualWord &w : words) {
      load(16, w.imp->getRVA());
      if (w.addend > 0 && w.addend < 4096) {
        put(0x91000210 | uint32_t(w.addend) << 10); // add x16, x16, #addend
      } else if (w.addend < 0 && w.addend > -4096) {
        put(0xD1000210 | uint32_t(-w.addend) << 10); // sub x16, x16, #-addend
      } else {
        // movz x17, #h0; movk x17, #hN, lsl #16N; add x16, x16, x17
        bool first = true;
        for (uint32_t hw = 0; hw != 4; ++hw) {
          uint32_t half = uint64_t(w.addend) >> (16 * hw) & 0xFFFF;
          if (!half && !(first && hw == 3))
            continue;
          put((first ? 0xD2800011 : 0xF2800011) | hw << 21 | half << 5);
          first = false;
        }
        put(0x8B110210);
      }
      address(17, w.chunk->getRVA() + w.offset);
      put(0xF9000230); // str x16, [x17]
    }
    if (protect) {
      // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
      // base, size and old at sp + 16, 24 and 32.
      address(0, sealStart);
      put(0xF9000BE0);                                    // str x0, [sp, #16]
      put(0xD2800001 | uint32_t(sealSize & 0xFFFF) << 5); // movz x1, #lo
      put(0xF2A00001 | uint32_t(sealSize >> 16 & 0xFFFF) << 5); // movk x1, #hi
      put(0xF9000FE1);                     // str x1, [sp, #24]
      put(0x92800000);                     // mov x0, #-1
      put(0x910043E1);                     // add x1, sp, #16
      put(0x910063E2);                     // add x2, sp, #24
      put(0x52800003 | pageReadOnly << 5); // mov w3, #PAGE_READONLY
      put(0x910083E4);                     // add x4, sp, #32
      load(16, protect->getRVA());
      put(0xD63F0200); // blr x16
      put(0x910003BF); // mov sp, x29
      put(0xA8C37BFD); // ldp x29, x30, [sp], #48
    } else {
      put(0x52800000); // mov w0, #0
    }
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put,
  // which ends with it.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  if (protect)
    put({0x48, 0x83, 0xEC, 0x48}); // sub rsp, 0x48
  // The epilogue, which a failed lookup of a host import jumps to with its
  // status in eax.
  uint32_t end = getSize() - 5;
  // LdrGetProcedureAddressForCaller(rcx, &name, 0, &cell, 1, fill), the name
  // an ANSI_STRING at rsp + 0x30. Flag 1 binds without initializing the
  // module, as the loader's snap does.
  auto lookup = [&](const RunTimeCell &c) {
    uint32_t len = c.name->getSize() - 1;
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + name]
    disp(c.name->getRVA());
    put({0x48, 0x89, 0x44, 0x24, 0x38});       // mov [rsp + 0x38], rax
    put({0xC7, 0x44, 0x24, 0x30, 0, 0, 0, 0}); // mov dword [rsp + 0x30], ...
    write32le(buf + off - 4, len | (len + 1) << 16);
    put({0x48, 0x8D, 0x54, 0x24, 0x30}); // lea rdx, [rsp + 0x30]
    put({0x45, 0x31, 0xC0});             // xor r8d, r8d
    put({0x4C, 0x8D, 0x0D, 0, 0, 0, 0}); // lea r9, [rip + cell]
    disp(c.cell->getRVA());
    put({0xC7, 0x44, 0x24, 0x20, 1, 0, 0, 0}); // mov dword [rsp + 0x20], 1
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0});       // lea rax, [rip + fill]
    disp(rva);
    put({0x48, 0x89, 0x44, 0x24, 0x28}); // mov [rsp + 0x28], rax
    put({0xFF, 0x15, 0, 0, 0,
         0}); // call [rip + LdrGetProcedureAddressForCaller]
    disp(getProc->getRVA());
  };
  for (const RunTimeCell &c : cells) {
    if (c.dllName) {
      // LdrGetDllHandleEx(1, null, null, &dllName, rsp + 0x40), the name a
      // UNICODE_STRING at rsp + 0x30, leaving the cell null when the DLL is
      // not loaded.
      uint32_t len = c.dllName->getSize() - 2;
      put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + dllName]
      disp(c.dllName->getRVA());
      put({0x48, 0x89, 0x44, 0x24, 0x38});       // mov [rsp + 0x38], rax
      put({0xC7, 0x44, 0x24, 0x30, 0, 0, 0, 0}); // mov dword [rsp + 0x30], ...
      write32le(buf + off - 4, len | (len + 2) << 16);
      put({0xB9, 1, 0, 0, 0});             // mov ecx, 1
      put({0x31, 0xD2});                   // xor edx, edx
      put({0x45, 0x31, 0xC0});             // xor r8d, r8d
      put({0x4C, 0x8D, 0x4C, 0x24, 0x30}); // lea r9, [rsp + 0x30]
      put({0x48, 0x8D, 0x44, 0x24, 0x40}); // lea rax, [rsp + 0x40]
      put({0x48, 0x89, 0x44, 0x24, 0x20}); // mov [rsp + 0x20], rax
      put({0xFF, 0x15, 0, 0, 0, 0});       // call [rip + LdrGetDllHandleEx]
      disp(getDll->getRVA());
      put({0x85, 0xC0});                   // test eax, eax
      put({0x78, 5 + 61});                 // js past the lookup
      put({0x48, 0x8B, 0x4C, 0x24, 0x40}); // mov rcx, [rsp + 0x40]
      lookup(c);
      continue;
    }
    put({0x65, 0x48, 0x8B, 0x0C, 0x25, 0x60, 0, 0, 0}); // mov rcx, gs:[0x60]
    put({0x48, 0x8B, 0x49, 0x10}); // mov rcx, [rcx + 0x10]: the executable
    lookup(c);
    if (!c.imp->file->isWeakImport) {
      put({0x85, 0xC0});             // test eax, eax
      put({0x0F, 0x88, 0, 0, 0, 0}); // js end
      write32le(buf + off - 4, end - off);
    }
  }
  for (const ResidualWord &w : words) {
    put({0x48, 0x8B, 0x05, 0, 0, 0, 0}); // mov rax, [rip + imp]
    disp(w.imp->getRVA());
    if (isInt<32>(w.addend)) {
      put({0x48, 0x05, 0, 0, 0, 0}); // add rax, addend
      write32le(buf + off - 4, uint32_t(w.addend));
    } else {
      put({0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0}); // mov rcx, addend
      write64le(buf + off - 8, uint64_t(w.addend));
      put({0x48, 0x01, 0xC8}); // add rax, rcx
    }
    put({0x48, 0x89, 0x05, 0, 0, 0, 0}); // mov [rip + word], rax
    disp(w.chunk->getRVA() + w.offset);
  }
  if (protect) {
    // NtProtectVirtualMemory(-1, &base, &size, PAGE_READONLY, &old), with
    // base, size and old at rsp + 0x30, 0x38 and 0x40.
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + base]
    disp(sealStart);
    put({0x48, 0x89, 0x44, 0x24, 0x30});             // mov [rsp + 0x30], rax
    put({0x48, 0xC7, 0x44, 0x24, 0x38, 0, 0, 0, 0}); // mov [rsp + 0x38], size
    write32le(buf + off - 4, uint32_t(sealSize));
    put({0x48, 0x8D, 0x44, 0x24, 0x40});               // lea rax, [rsp + 0x40]
    put({0x48, 0x89, 0x44, 0x24, 0x20});               // mov [rsp + 0x20], rax
    put({0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF});   // mov rcx, -1
    put({0x48, 0x8D, 0x54, 0x24, 0x30});               // lea rdx, [rsp + 0x30]
    put({0x4C, 0x8D, 0x44, 0x24, 0x38});               // lea r8, [rsp + 0x38]
    put({0x41, 0xB9, uint8_t(pageReadOnly), 0, 0, 0}); // mov r9d, PAGE_READONLY
    put({0xFF, 0x15, 0, 0, 0, 0}); // call [rip + NtProtectVirtualMemory]
    disp(protect->getRVA());
    put({0x48, 0x83, 0xC4, 0x48}); // add rsp, 0x48
  } else {
    put({0x31, 0xC0}); // xor eax, eax
  }
  put({0xC3}); // ret
}

void ResidualFillUnwindChunk::writeTo(uint8_t *buf) const {
  if (ctx.config.machine == ARM64) {
    // One epilog at the end, which the prologue's codes describe: set_fp,
    // save_fplr_x of 48 bytes, end.
    write32le(buf, fill->getSize() / 4 | 1 << 21 | 1 << 27);
    const uint8_t codes[] = {0xE1, 0x85, 0xE4, 0xE4};
    memcpy(buf + 4, codes, sizeof(codes));
    return;
  }
  // Version 1, a prologue of 4 bytes with one code: UWOP_ALLOC_SMALL of
  // 0x48 bytes at its end.
  const uint8_t info[] = {0x01, 0x04, 0x01, 0x00, 0x04, 0x82, 0x00, 0x00};
  memcpy(buf, info, sizeof(info));
}

size_t ResidualFillPdataChunk::getSize() const {
  return ctx.config.machine == ARM64 ? 8 : 12;
}

void ResidualFillPdataChunk::writeTo(uint8_t *buf) const {
  write32le(buf, fill->getRVA());
  if (ctx.config.machine == ARM64) {
    write32le(buf + 4, unwind->getRVA());
    return;
  }
  write32le(buf + 4, fill->getRVA() + fill->getSize());
  write32le(buf + 8, unwind->getRVA());
}

// A word of this image that holds the address of an overridable default X: an
// in-place import slot of X, or the entry through which the image calls X and
// loads its address. The loader writes it with the runtime's entry; the fill
// writes the program's replacement over it when the executable replaces X.
struct OverrideCell {
  // The cell's chunk and offset, or null for the import's entry.
  Chunk *chunk;
  uint32_t offset;
  // The import of the default, or null for a cell of the image's own default.
  DefinedImportData *imp;
  // The default's slot in the table, or, for a default replaced by name, its
  // key in the table's block, which is never zero.
  uint32_t slot;
  uint64_t key;
  bool readOnly;
  // Whether the slot is in the template of the image's thread-local data.
  bool threadLocal;

  uint64_t getRVA() const {
    return chunk ? chunk->getRVA() + offset : imp->getRVA();
  }
};

// Where the TLS directory says the template of the image's thread-local data
// starts, and the index the loader writes for the image's thread-local blocks:
// a symbol and an addend each.
struct TLSTemplate {
  Defined *start = nullptr;
  int64_t startAddend = 0;
  Defined *index = nullptr;
  int64_t indexAddend = 0;

  uint64_t getIndexRVA() const { return index->getRVA() + indexAddend; }
  // The offset in each thread's block of the template word at rva.
  uint32_t getOffset(uint64_t rva) const {
    return rva - (start->getRVA() + startAddend);
  }
};

// The fill of a DLL's overridable-default cells, which the C initializer table
// runs once, before every other initializer and before the residual fill. It
// reads the executable's override table through the PEB and, for each cell
// whose slot the anchor's mask names replaced, writes the executable's image
// base plus the slot's RVA, the replacement, into the cell. A forwarded or
// unreplaced slot, or a foreign executable with no table, leaves the cell the
// loader's value, the runtime's entry, so a program that replaces nothing
// writes nothing. A cell of a default replaced by name takes the address that
// the anchor's entry with its key names, if any. The read-only cells are
// inside the import address table directory, which the loader restores to
// read-only once it has bound the image; when one of their slots is replaced,
// or for a default replaced by name the table has a block, the fill makes the
// range writable with NtProtectVirtualMemory, writes them, and restores the
// protection the call reported, as the loader's own bracket does. Those calls
// give it a frame; without read-only cells it is a leaf. It returns 0, or the
// status of a protection call that failed, which fails the image's start-up, as
// any nonzero return from the table does.
//
// The loader binds a DLL's thread-local template before it copies it into the
// block of any thread, so an unreplaced thread-local cell is right in every
// thread. A replaced one is written in the template, which every thread created
// later copies, and in the loading thread's block, which the loader copied
// before the fill ran. A thread that already ran when the DLL was loaded late
// got its block from the template before the fill, and keeps the runtime's
// entry, through which a call still reaches the replacement.
class OverrideCellFillChunk : public NonSectionCodeChunk {
public:
  OverrideCellFillChunk(COFFLinkerContext &ctx, std::vector<OverrideCell> cells,
                        DefinedImportData *protect, TLSTemplate tls)
      : cells(std::move(cells)), protect(protect), tls(tls), ctx(ctx) {
    setAlignment(16);
  }
  size_t getSize() const override;
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override { return ctx.config.machine; }

private:
  size_t getCellSize(const OverrideCell &c) const;
  // The size of the test of whether a read-only cell is to be written: by its
  // slot's bit in the mask, or, for a default replaced by name, by whether the
  // table has a block.
  size_t getReadOnlyTestSize() const;
  uint32_t getReadOnlyMask() const;
  bool hasReadOnlyByName() const {
    return llvm::any_of(
        cells, [](const OverrideCell &c) { return c.readOnly && c.key; });
  }

  // The writable cells, then the read-only ones.
  std::vector<OverrideCell> cells;
  // NtProtectVirtualMemory's import, when there are read-only cells.
  DefinedImportData *protect;
  // The TLS directory's template and index, when there are thread-local cells.
  TLSTemplate tls;
  COFFLinkerContext &ctx;
};

// The table's anchor RVA follows its magic, version and count, and its block's
// RVA the anchor's; the anchor's mask follows its own magic, version and count,
// and the number of the block's entries the mask.
static constexpr uint32_t overrideAnchorRVAOffset = overrideHeaderOffset + 8;
static constexpr uint32_t overrideBlockRVAOffset = overrideHeaderOffset + 12;
static constexpr uint32_t overrideCountOffset = overrideHeaderOffset + 6;
static constexpr uint32_t overrideAnchorMaskOffset = 8;
static constexpr uint32_t overrideAnchorEntriesOffset = 12;

size_t OverrideCellFillChunk::getCellSize(const OverrideCell &c) const {
  if (ctx.config.machine == ARM64) {
    // By name, the search of the anchor's entries: ldr; add; ldr; ldrh; add;
    // add; movz and three movk; cbz; ldr; cmp; b.eq; add; sub; b; ldr; then
    // adrp; add; str. By slot: tbz; ldr; add; adrp; add; str. For a
    // thread-local cell, the store in the thread's block: adrp; ldr; ldr; ldr;
    // movz; movk; str.
    return (c.key ? 72 : 12) + 12 + (c.threadLocal ? 28 : 0);
  }
  // By name, the search of the anchor's entries: mov eax, [r11 + anchor];
  // add rax, r11; mov ecx, [rax + entries]; movzx edx, word [r11 + count];
  // lea rax, [rax + rdx * 8 + 16]; mov rdx, key; test ecx, ecx; jz .next;
  // cmp [rax], rdx; je .found; add rax, 16; dec ecx; jmp; mov rax, [rax + 8].
  // By slot: bt; jnc; mov eax, [r11 + rva]; add rax, r11. Then
  // mov [rip + cell], rax; for a thread-local cell, the store in the thread's
  // block: mov ecx, [rip + index]; mov rdx, gs:[0x58];
  // mov rdx, [rdx + rcx * 8]; mov [rdx + offset], rax
  size_t find =
      c.key ? 57 : 5 + 2 + (overrideSlotOffset(c.slot) + 4 >= 0x80 ? 7 : 4) + 3;
  return find + 7 + (c.threadLocal ? 26 : 0);
}

uint32_t OverrideCellFillChunk::getReadOnlyMask() const {
  uint32_t mask = 0;
  for (const OverrideCell &c : cells)
    if (c.readOnly && !c.key)
      mask |= 1u << c.slot;
  return mask;
}

size_t OverrideCellFillChunk::getReadOnlyTestSize() const {
  bool byName = hasReadOnlyByName();
  bool bySlot = getReadOnlyMask() != 0;
  // movz; movk; tst; b.eq or b.ne; then by name, ldr; cbz.
  if (ctx.config.machine == ARM64)
    return (bySlot ? 16 : 0) + (byName ? 8 : 0);
  // test r10d, mask; je, or by name jne rel8; then cmp [r11 + block], 0; je.
  if (!byName)
    return 13;
  return (bySlot ? 9 : 0) + 14;
}

size_t OverrideCellFillChunk::getSize() const {
  size_t size = 0;
  for (const OverrideCell &c : cells)
    size += getCellSize(c);
  if (ctx.config.machine == ARM64) {
    // The base and the magic's check (seven words), the mask (three), the
    // zero status and ret; with read-only cells, the frame (four), the test of
    // their slots (four), the two protection calls (fourteen and eight), the
    // first's status check, the reload of the base and mask between them (five)
    // and the branch past the zero status.
    size += 28 + 12 + 8;
    return protect ? size + 16 + getReadOnlyTestSize() + 56 + 32 + 4 + 20 + 4
                   : size;
  }
  // The base, the magic's check, the mask, the zero status and ret; with
  // read-only cells, the frame, the test of their slots, the two protection
  // calls, the first's status check, the reload of the base and mask between
  // them and the jump past the zero status.
  size += 13 + 17 + 12 + 2 + 1;
  return protect ? size + 8 + getReadOnlyTestSize() + 60 + 38 + 8 + 25 + 2
                 : size;
}

void OverrideCellFillChunk::writeTo(uint8_t *buf) const {
  const uint32_t pageReadWrite = 4;
  uint32_t roMask = getReadOnlyMask();
  bool roByName = hasReadOnlyByName();
  uint64_t roStart = UINT64_MAX, roEnd = 0;
  for (const OverrideCell &c : cells) {
    if (!c.readOnly)
      continue;
    roStart = std::min(roStart, c.getRVA());
    roEnd = std::max(roEnd, c.getRVA() + ctx.config.wordsize);
  }
  uint32_t off = 0;
  // The branches to the zero status, and to the epilogue with the status of a
  // protection call, patched once their places are known.
  SmallVector<uint32_t, 4> toZero, toDone;

  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    auto branch = [&](SmallVector<uint32_t, 4> &list, uint32_t insn) {
      list.push_back(off);
      put(insn);
    };
    // A conditional branch's or cbnz's 19-bit offset, or b's 26-bit one.
    auto patch = [&](ArrayRef<uint32_t> list, uint32_t target) {
      for (uint32_t b : list) {
        uint32_t insn = read32le(buf + b);
        uint32_t delta = (target - b) / 4;
        write32le(buf + b, (insn >> 26) == 0x05
                               ? insn | (delta & 0x3FFFFFF)
                               : insn | (delta & 0x7FFFF) << 5);
      }
    };
    // movz wN, #lo; movk wN, #hi, lsl 16
    auto movImm = [&](uint32_t reg, uint32_t value) {
      put(0x52800000 | reg | (value & 0xFFFF) << 5);
      put(0x72A00000 | reg | (value >> 16 & 0xFFFF) << 5);
    };
    // adrp xN, target; add xN, xN, :lo12:target
    auto address = [&](uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, 0x91000000 | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Imm(buf + off + 4, target & 0xfff, 0);
      off += 8;
    };
    // adrp x16, protect; ldr x16, [x16, :lo12:protect]; blr x16
    auto callProtect = [&] {
      write32le(buf + off, 0x90000010);
      write32le(buf + off + 4, 0xF9400210);
      applyArm64Addr(buf + off, protect->getRVA(), rva + off, 12);
      applyArm64Ldr(buf + off + 4, protect->getRVA() & 0xfff);
      off += 8;
      put(0xD63F0200);
    };
    // The executable's image base, from the PEB, into x16.
    auto loadBase = [&] {
      put(0xF9403250); // ldr x16, [x18, #0x60]   TEB->PEB
      put(0xF9400A10); // ldr x16, [x16, #0x10]   PEB->ImageBaseAddress
    };
    // The anchor's mask into w10: ldr w10, [x16, #anchor]; add x10, x16, x10;
    // ldr w10, [x10, #mask].
    auto loadMask = [&] {
      put(0xB9400000 | overrideAnchorRVAOffset / 4 << 10 | 16 << 5 | 10);
      put(0x8B0A020A);
      put(0xB9400000 | overrideAnchorMaskOffset / 4 << 10 | 10 << 5 | 10);
    };
    auto writeCells = [&](bool readOnly) {
      for (const OverrideCell &c : cells) {
        if (c.readOnly != readOnly)
          continue;
        uint32_t skip;
        if (c.key) {
          // The anchor's entries into x11 and their number into w12:
          // ldr w11, [x16, #anchor]; add x11, x16, x11; ldr w12, [x11, #n];
          // ldrh w13, [x16, #count]; add x11, x11, x13, lsl #3;
          // add x11, x11, #16. The key into x14. Then the search:
          // .loop: cbz w12, .next; ldr x13, [x11]; cmp x13, x14; b.eq .found;
          // add x11, x11, #16; sub w12, w12, #1; b .loop;
          // .found: ldr x17, [x11, #8]
          put(0xB9400000 | overrideAnchorRVAOffset / 4 << 10 | 16 << 5 | 11);
          put(0x8B0B020B);
          put(0xB9400000 | overrideAnchorEntriesOffset / 4 << 10 | 11 << 5 |
              12);
          put(0x79400000 | overrideCountOffset / 2 << 10 | 16 << 5 | 13);
          put(0x8B0D0D6B);
          put(0x91000000 | overrideAnchorHeaderSize << 10 | 11 << 5 | 11);
          put(0xD280000E | uint32_t(c.key & 0xFFFF) << 5);
          for (uint32_t hw = 1; hw != 4; ++hw)
            put(0xF280000E | hw << 21 |
                uint32_t(c.key >> (16 * hw) & 0xFFFF) << 5);
          skip = off;
          put(0x3400000C);
          put(0xF940016D);
          put(0xEB0E01BF);
          put(0x54000080); // b.eq +4
          put(0x91000000 | 16 << 10 | 11 << 5 | 11);
          put(0x5100058C);
          put(0x14000000 | (-6 & 0x3FFFFFF));
          put(0xF9400571);
        } else {
          // tbz w10, #slot, .next; ldr w17, [x16, #rva]; add x17, x16, x17
          skip = off;
          put(0x36000000 | c.slot << 19 | 10);
          put(0xB9400000 | (overrideSlotOffset(c.slot) + 4) / 4 << 10 |
              16 << 5 | 17);
          put(0x8B110211);
        }
        // adrp x9, cell; add x9, x9, :lo12:cell; str x17, [x9]
        address(9, c.getRVA());
        put(0xF9000131);
        if (c.threadLocal) {
          // adrp x9, index; ldr w9, [x9, :lo12:index]; ldr x11, [x18, #0x58]
          // (TEB->ThreadLocalStoragePointer); ldr x11, [x11, x9, lsl #3];
          // movz/movk w12, #offset; str x17, [x11, x12]
          uint64_t index = tls.getIndexRVA();
          write32le(buf + off, 0x90000009);
          write32le(buf + off + 4, 0xB9400129);
          applyArm64Addr(buf + off, index, rva + off, 12);
          applyArm64Ldr(buf + off + 4, index & 0xfff);
          off += 8;
          put(0xF9402E4B);
          put(0xF869796B);
          movImm(12, tls.getOffset(c.getRVA()));
          put(0xF82C6971);
        }
        write32le(buf + skip, read32le(buf + skip) | (off - skip) / 4 << 5);
      }
    };

    if (protect) {
      put(0xA9BD7BFD); // stp x29, x30, [sp, #-48]!
      put(0x910003FD); // mov x29, sp
    }
    loadBase();
    put(0xB9408211); // ldr w17, [x16, #0x80]   the table's magic
    movImm(9, overrideTableMagic);
    put(0x6B09023F);            // cmp w17, w9
    branch(toZero, 0x54000001); // b.ne .zero
    loadMask();
    writeCells(false);
    if (protect) {
      // A read-only cell of a default replaced by name is written whenever
      // the table has a block.
      if (roMask) {
        movImm(9, roMask);
        put(0x6A09015F); // tst w10, w9
        if (roByName)
          put(0x54000061); // b.ne +3
        else
          branch(toZero, 0x54000000); // b.eq .zero
      }
      if (roByName) {
        put(0xB9400000 | overrideBlockRVAOffset / 4 << 10 | 16 << 5 | 9);
        branch(toZero, 0x34000009); // cbz w9, .zero
      }
      // NtProtectVirtualMemory(-1, &base, &size, PAGE_READWRITE, &old), with
      // base, size and old at sp + 16, 24 and 32.
      address(0, roStart);
      put(0xF9000BE0); // str x0, [sp, #16]
      put(0xD2800001 | uint32_t((roEnd - roStart) & 0xFFFF) << 5); // movz x1
      put(0xF2A00001 | uint32_t((roEnd - roStart) >> 16 & 0xFFFF) << 5);
      put(0xF9000FE1);                      // str x1, [sp, #24]
      put(0x92800000);                      // mov x0, #-1
      put(0x910043E1);                      // add x1, sp, #16
      put(0x910063E2);                      // add x2, sp, #24
      put(0x52800003 | pageReadWrite << 5); // mov w3, #PAGE_READWRITE
      put(0x910083E4);                      // add x4, sp, #32
      callProtect();
      branch(toDone, 0x35000000); // cbnz w0, .done
      loadBase();
      loadMask();
      writeCells(true);
      // NtProtectVirtualMemory(-1, &base, &size, old, &old) over the pages the
      // first call reported.
      put(0xB94023E3); // ldr w3, [sp, #32]
      put(0x92800000); // mov x0, #-1
      put(0x910043E1); // add x1, sp, #16
      put(0x910063E2); // add x2, sp, #24
      put(0x910083E4); // add x4, sp, #32
      callProtect();
      branch(toDone, 0x14000000); // b .done
    }
    patch(toZero, off);
    put(0x52800000); // mov w0, #0
    patch(toDone, off);
    if (protect) {
      put(0x910003BF); // mov sp, x29
      put(0xA8C37BFD); // ldp x29, x30, [sp], #48
    }
    put(0xD65F03C0); // ret
    assert(off == getSize());
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  auto jump = [&](SmallVector<uint32_t, 4> &list, uint8_t cc) {
    put({0x0F, cc, 0, 0, 0, 0});
    list.push_back(off - 4);
  };
  auto patch = [&](ArrayRef<uint32_t> list) {
    for (uint32_t d : list)
      write32le(buf + d, off - (d + 4));
  };
  // The executable's image base, from the PEB, into r11.
  auto loadBase = [&] {
    put({0x65, 0x4C, 0x8B, 0x1C, 0x25, 0x60, 0, 0, 0}); // mov r11, gs:[0x60]
    put({0x4D, 0x8B, 0x5B, 0x10});                      // mov r11, [r11 + 0x10]
  };
  // The anchor's mask into r10d.
  auto loadMask = [&] {
    put({0x45, 0x8B, 0x93, 0, 0, 0, 0}); // mov r10d, [r11 + anchor]
    write32le(buf + off - 4, overrideAnchorRVAOffset);
    // mov r10d, [r11 + r10 + mask]
    put({0x47, 0x8B, 0x54, 0x13, uint8_t(overrideAnchorMaskOffset)});
  };
  auto writeCells = [&](bool readOnly) {
    for (const OverrideCell &c : cells) {
      if (c.readOnly != readOnly)
        continue;
      uint32_t skip;
      if (c.key) {
        put({0x41, 0x8B, 0x83, 0, 0, 0, 0}); // mov eax, [r11 + anchor]
        write32le(buf + off - 4, overrideAnchorRVAOffset);
        put({0x4C, 0x01, 0xD8}); // add rax, r11
        // mov ecx, [rax + entries]   the number of the block's entries
        put({0x8B, 0x48, uint8_t(overrideAnchorEntriesOffset)});
        put({0x41, 0x0F, 0xB7, 0x93, 0, 0, 0, 0}); // movzx edx, [r11 + count]
        write32le(buf + off - 4, overrideCountOffset);
        // lea rax, [rax + rdx * 8 + 16]   the anchor's first entry
        put({0x48, 0x8D, 0x44, 0xD0, uint8_t(overrideAnchorHeaderSize)});
        put({0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0}); // mov rdx, key
        write64le(buf + off - 8, c.key);
        uint32_t loop = off;
        put({0x85, 0xC9}); // test ecx, ecx
        put({0x74, 0});    // jz .next
        skip = off - 1;
        put({0x48, 0x39, 0x10});       // cmp [rax], rdx
        put({0x74, 8});                // je .found
        put({0x48, 0x83, 0xC0, 0x10}); // add rax, 16
        put({0xFF, 0xC9});             // dec ecx
        put({0xEB, 0});                // jmp .loop
        buf[off - 1] = uint8_t(loop - off);
        put({0x48, 0x8B, 0x40, 0x08}); // .found: mov rax, [rax + 8]
      } else {
        put({0x41, 0x0F, 0xBA, 0xE2, uint8_t(c.slot)}); // bt r10d, slot
        put({0x73, 0});                                 // jnc .next
        skip = off - 1;
        // mov eax, dword ptr [r11 + slot offset + 4]   the slot's RVA
        uint32_t rvaOff = overrideSlotOffset(c.slot) + 4;
        if (rvaOff >= 0x80) {
          put({0x41, 0x8B, 0x83, 0, 0, 0, 0});
          write32le(buf + off - 4, rvaOff);
        } else {
          put({0x41, 0x8B, 0x43, uint8_t(rvaOff)});
        }
        put({0x4C, 0x01, 0xD8}); // add rax, r11   base + rva
      }
      put({0x48, 0x89, 0x05, 0, 0, 0, 0}); // mov [rip + cell], rax
      disp(c.getRVA());
      if (c.threadLocal) {
        put({0x8B, 0x0D, 0, 0, 0, 0}); // mov ecx, [rip + index]
        disp(tls.getIndexRVA());
        // mov rdx, gs:[0x58]   TEB->ThreadLocalStoragePointer
        put({0x65, 0x48, 0x8B, 0x14, 0x25, 0x58, 0, 0, 0});
        put({0x48, 0x8B, 0x14, 0xCA});       // mov rdx, [rdx + rcx * 8]
        put({0x48, 0x89, 0x82, 0, 0, 0, 0}); // mov [rdx + offset], rax
        write32le(buf + off - 4, tls.getOffset(c.getRVA()));
      }
      buf[skip] = off - (skip + 1);
    }
  };
  // NtProtectVirtualMemory(-1, &base, &size, r9d, &old), with base, size and
  // old at rsp + 0x30, 0x38 and 0x40.
  auto callProtect = [&] {
    put({0x48, 0x8D, 0x44, 0x24, 0x40});             // lea rax, [rsp + 0x40]
    put({0x48, 0x89, 0x44, 0x24, 0x20});             // mov [rsp + 0x20], rax
    put({0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF}); // mov rcx, -1
    put({0x48, 0x8D, 0x54, 0x24, 0x30});             // lea rdx, [rsp + 0x30]
    put({0x4C, 0x8D, 0x44, 0x24, 0x38});             // lea r8, [rsp + 0x38]
    put({0xFF, 0x15, 0, 0, 0, 0}); // call [rip + NtProtectVirtualMemory]
    disp(protect->getRVA());
  };

  if (protect)
    put({0x48, 0x83, 0xEC, 0x48}); // sub rsp, 0x48
  loadBase();
  // The magic is at 0x80, past the reach of a signed disp8, so a disp32.
  put({0x41, 0x81, 0xBB, 0x80, 0, 0, 0, 0, 0, 0, 0}); // cmp [r11 + 0x80], magic
  write32le(buf + off - 4, overrideTableMagic);
  jump(toZero, 0x85); // jne .zero
  loadMask();
  writeCells(false);
  if (protect) {
    // A read-only cell of a default replaced by name is written whenever the
    // table has a block.
    if (roMask || !roByName) {
      put({0x41, 0xF7, 0xC2, 0, 0, 0, 0}); // test r10d, read-only cells' slots
      write32le(buf + off - 4, roMask);
      if (roByName)
        put({0x75, 14}); // jne past the block's test
      else
        jump(toZero, 0x84); // je .zero
    }
    if (roByName) {
      put({0x41, 0x83, 0xBB, 0, 0, 0, 0, 0}); // cmp dword [r11 + block], 0
      write32le(buf + off - 5, overrideBlockRVAOffset);
      jump(toZero, 0x84); // je .zero
    }
    put({0x48, 0x8D, 0x05, 0, 0, 0, 0}); // lea rax, [rip + base]
    disp(roStart);
    put({0x48, 0x89, 0x44, 0x24, 0x30});             // mov [rsp + 0x30], rax
    put({0x48, 0xC7, 0x44, 0x24, 0x38, 0, 0, 0, 0}); // mov [rsp + 0x38], size
    write32le(buf + off - 4, uint32_t(roEnd - roStart));
    // mov r9d, PAGE_READWRITE
    put({0x41, 0xB9, uint8_t(pageReadWrite), 0, 0, 0});
    callProtect();
    put({0x85, 0xC0});  // test eax, eax
    jump(toDone, 0x85); // jne .done
    loadBase();
    loadMask();
    writeCells(true);
    // The protection the first call reported, over the pages it reported.
    put({0x44, 0x8B, 0x4C, 0x24, 0x40}); // mov r9d, [rsp + 0x40]
    callProtect();
    put({0xEB, 2}); // jmp .done
  }
  patch(toZero);
  put({0x31, 0xC0}); // xor eax, eax
  patch(toDone);
  if (protect)
    put({0x48, 0x83, 0xC4, 0x48}); // add rsp, 0x48
  put({0xC3});                     // ret
  assert(off == getSize());
}

// The copy of an executable's thread-local import slots into the main thread's
// block. The loader copies the executable's template into the main thread's
// block before it binds the executable's imports, and only then initializes
// the DLLs and calls the executable's TLS callbacks, so the main thread's block
// holds each slot's lookup entry while every later thread copies the bound
// template. The copy runs as the executable's first TLS callback, and the
// executable names it in its header for the code of a DLL that may reach the
// executable while the DLLs initialize to run first. It acts only while the
// process initializes, when no thread but the main one runs, and writes only a
// word that still holds its lookup entry, so it may run any number of times and
// never undoes an assignment. It is a leaf that returns 0.
class ThreadLocalSlotCopyChunk : public NonSectionCodeChunk {
public:
  ThreadLocalSlotCopyChunk(COFFLinkerContext &ctx,
                           std::vector<const ImportSlot *> slots,
                           TLSTemplate tls)
      : slots(std::move(slots)), tls(tls), ctx(ctx) {
    setAlignment(16);
  }
  size_t getSize() const override {
    // The test of the process's initialization and the block's address; for
    // each word, its test and its copy; then the zero status and ret.
    if (ctx.config.machine == ARM64)
      return 28 + 48 * slots.size() + 8;
    return 39 + 36 * slots.size() + 3;
  }
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override { return ctx.config.machine; }

private:
  // The slots in the template, each with its lookup entry.
  std::vector<const ImportSlot *> slots;
  // The value of a slot's lookup entry, which the slot holds in the file.
  uint64_t getLookupValue(const ImportSlot *s) const {
    uint8_t value[8] = {};
    s->lookup->writeTo(value);
    return read64le(value);
  }
  TLSTemplate tls;
  COFFLinkerContext &ctx;
};

// PEB->CrossProcessFlags, whose bit 1 the loader sets while it initializes the
// process.
static constexpr uint32_t pebCrossProcessFlags = 0x50;

void ThreadLocalSlotCopyChunk::writeTo(uint8_t *buf) const {
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    // adrp xN, target; ldr wN or xN, [xN, :lo12:target]
    auto load = [&](uint32_t ldr, uint32_t reg, uint64_t target) {
      write32le(buf + off, 0x90000000 | reg);
      write32le(buf + off + 4, ldr | reg << 5 | reg);
      applyArm64Addr(buf + off, target, rva + off, 12);
      applyArm64Ldr(buf + off + 4, target & 0xfff);
      off += 8;
    };
    put(0xF9403249); // ldr x9, [x18, #0x60]   TEB->PEB
    put(0xB9400129 | pebCrossProcessFlags / 4 << 10); // ldr w9, [x9, #flags]
    uint32_t skip = off;
    put(0x36080009);                        // tbz w9, #1, .done
    load(0xB9400000, 9, tls.getIndexRVA()); // the index into w9
    put(0xF9402E4B); // ldr x11, [x18, #0x58]   TEB->ThreadLocalStoragePointer
    put(0xF869796B); // ldr x11, [x11, x9, lsl #3]
    for (const ImportSlot *s : slots) {
      // movz/movk w12, #offset; ldr x13, [x11, x12]; movz/movk x14, #lookup
      // value; cmp x13, x14; b.ne .next; the word into x10;
      // str x10, [x11, x12]
      uint64_t word = s->chunk->getRVA() + s->offset;
      uint32_t blockOff = tls.getOffset(word);
      put(0x5280000C | (blockOff & 0xFFFF) << 5);
      put(0x72A0000C | (blockOff >> 16) << 5);
      put(0xF86C696D);
      uint64_t value = getLookupValue(s);
      put(0xD280000E | uint32_t(value & 0xFFFF) << 5);
      for (uint32_t hw = 1; hw != 4; ++hw)
        put(0xF280000E | hw << 21 | uint32_t(value >> (16 * hw) & 0xFFFF) << 5);
      put(0xEB0E01BF);
      put(0x54000081);
      load(0xF9400000, 10, word);
      put(0xF82C696A);
    }
    write32le(buf + skip, read32le(buf + skip) | (off - skip) / 4 << 5);
    put(0x52800000); // mov w0, #0
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  // The RIP-relative displacement to target of the instruction just put.
  auto disp = [&](uint64_t target) {
    write32le(buf + off - 4, target - (rva + off));
  };
  put({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0, 0, 0}); // mov rax, gs:[0x60]
  // bt dword ptr [rax + flags], 1; jnc .done
  put({0x0F, 0xBA, 0x60, uint8_t(pebCrossProcessFlags), 1});
  put({0x0F, 0x83, 0, 0, 0, 0});
  uint32_t skip = off - 4;
  put({0x8B, 0x0D, 0, 0, 0, 0}); // mov ecx, [rip + index]
  disp(tls.getIndexRVA());
  // mov rdx, gs:[0x58]   TEB->ThreadLocalStoragePointer
  put({0x65, 0x48, 0x8B, 0x14, 0x25, 0x58, 0, 0, 0});
  put({0x48, 0x8B, 0x14, 0xCA}); // mov rdx, [rdx + rcx * 8]
  for (const ImportSlot *s : slots) {
    uint64_t word = s->chunk->getRVA() + s->offset;
    uint32_t blockOff = tls.getOffset(word);
    put({0x48, 0x8B, 0x82, 0, 0, 0, 0}); // mov rax, [rdx + offset]
    write32le(buf + off - 4, blockOff);
    put({0x49, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}); // mov r8, lookup value
    write64le(buf + off - 8, getLookupValue(s));
    put({0x4C, 0x39, 0xC0});             // cmp rax, r8
    put({0x75, 14});                     // jne .next
    put({0x48, 0x8B, 0x05, 0, 0, 0, 0}); // mov rax, [rip + word]
    disp(word);
    put({0x48, 0x89, 0x82, 0, 0, 0, 0}); // mov [rdx + offset], rax
    write32le(buf + off - 4, blockOff);
  }
  write32le(buf + skip, off - (skip + 4));
  put({0x31, 0xC0}); // xor eax, eax
  put({0xC3});       // ret
}

// The run of the executable's thread-local slot copy by a DLL that may reach
// the executable while the DLLs initialize, which the C initializer table runs
// before every other initializer of the DLL. While the process initializes, it
// reads the executable's MZ header through the PEB and, when the header names a
// copy, calls it, checked by Control Flow Guard where the image has the check,
// so that code of the executable that the DLL's initializers call sees its
// thread-local import slots bound in the main thread. It returns 0, as the copy
// does. On x86-64 it tail-jumps through the guard's dispatch and needs no
// frame; on AArch64 the check is a call, so it keeps the fill's frame.
class ThreadLocalSlotCopyCallChunk : public NonSectionCodeChunk {
public:
  ThreadLocalSlotCopyCallChunk(COFFLinkerContext &ctx, Defined *guard)
      : guard(guard), ctx(ctx) {
    setAlignment(16);
  }
  size_t getSize() const override {
    if (ctx.config.machine == ARM64)
      return 72 + (guard ? 12 : 0);
    return 40 + (guard ? 4 : 0);
  }
  void writeTo(uint8_t *buf) const override;
  uint32_t getOutputCharacteristics() const override {
    return llvm::COFF::IMAGE_SCN_CNT_CODE |
           NonSectionCodeChunk::getOutputCharacteristics();
  }
  StringRef getSectionName() const override { return ".text"; }
  MachineTypes getMachine() const override { return ctx.config.machine; }

private:
  // The guard's dispatch pointer on x86-64, or its check pointer on AArch64,
  // or null in an image without them.
  Defined *guard;
  COFFLinkerContext &ctx;
};

void ThreadLocalSlotCopyCallChunk::writeTo(uint8_t *buf) const {
  uint32_t off = 0;
  if (ctx.config.machine == ARM64) {
    auto put = [&](uint32_t insn) {
      write32le(buf + off, insn);
      off += 4;
    };
    put(0xA9BD7BFD); // stp x29, x30, [sp, #-48]!
    put(0x910003FD); // mov x29, sp
    put(0xF9403249); // ldr x9, [x18, #0x60]   TEB->PEB
    put(0xB940012A | pebCrossProcessFlags / 4 << 10); // ldr w10, [x9, #flags]
    uint32_t notInitializing = off;
    put(0x3608000A); // tbz w10, #1, .zero
    put(0xF9400929); // ldr x9, [x9, #0x10]   PEB->ImageBaseAddress
    // ldr w10, [x9, #tag]; movz/movk w11, #tag; cmp w10, w11; b.ne .zero
    put(0xB940012A | threadLocalSlotCopyOffset / 4 << 10);
    put(0x5280000B | (threadLocalSlotCopyTag & 0xFFFF) << 5);
    put(0x72A0000B | (threadLocalSlotCopyTag >> 16) << 5);
    put(0x6B0B015F);
    uint32_t noCopy = off;
    put(0x54000001);
    // ldr w10, [x9, #rva]; add x15, x9, x10
    put(0xB940012A | (threadLocalSlotCopyOffset + 4) / 4 << 10);
    put(0x8B0A012F);
    if (guard) {
      // adrp x16, guard; ldr x16, [x16, :lo12:guard]; blr x16, which checks
      // the target in x15 and preserves it.
      write32le(buf + off, 0x90000010);
      write32le(buf + off + 4, 0xF9400210);
      applyArm64Addr(buf + off, guard->getRVA(), rva + off, 12);
      applyArm64Ldr(buf + off + 4, guard->getRVA() & 0xfff);
      off += 8;
      put(0xD63F0200);
    }
    put(0xD63F01E0); // blr x15
    write32le(buf + notInitializing, read32le(buf + notInitializing) |
                                         (off - notInitializing) / 4 << 5);
    write32le(buf + noCopy, read32le(buf + noCopy) | (off - noCopy) / 4 << 5);
    put(0x52800000); // mov w0, #0
    put(0x910003BF); // mov sp, x29
    put(0xA8C37BFD); // ldp x29, x30, [sp], #48
    put(0xD65F03C0); // ret
    return;
  }

  auto put = [&](ArrayRef<uint8_t> bytes) {
    memcpy(buf + off, bytes.data(), bytes.size());
    off += bytes.size();
  };
  put({0x65, 0x48, 0x8B, 0x04, 0x25, 0x60, 0, 0, 0}); // mov rax, gs:[0x60]
  // bt dword ptr [rax + flags], 1; jnc .zero
  put({0x0F, 0xBA, 0x60, uint8_t(pebCrossProcessFlags), 1});
  put({0x73, 0});
  uint32_t notInitializing = off - 1;
  put({0x48, 0x8B, 0x40, 0x10}); // mov rax, [rax + 0x10]   ImageBaseAddress
  // cmp dword ptr [rax + tag], tag; jne .zero
  put({0x81, 0x78, uint8_t(threadLocalSlotCopyOffset), 0, 0, 0, 0});
  write32le(buf + off - 4, threadLocalSlotCopyTag);
  put({0x75, 0});
  uint32_t noCopy = off - 1;
  // mov ecx, [rax + rva]; add rax, rcx
  put({0x8B, 0x48, uint8_t(threadLocalSlotCopyOffset + 4)});
  put({0x48, 0x01, 0xC8});
  if (guard) {
    // jmp [rip + __guard_dispatch_icall_fptr], which checks the target in rax
    // and jumps to it.
    put({0xFF, 0x25, 0, 0, 0, 0});
    write32le(buf + off - 4, guard->getRVA() - (rva + off));
  } else {
    put({0xFF, 0xE0}); // jmp rax
  }
  buf[notInitializing] = off - (notInitializing + 1);
  buf[noCopy] = off - (noCopy + 1);
  put({0x31, 0xC0}); // xor eax, eax
  put({0xC3});       // ret
}
} // namespace

// Whether a section is unwind or exception-handling data, which the PE format
// defines as RVAs of the image's own code and handlers.
static bool isExceptionData(SectionChunk *sc) {
  StringRef name = sc->getSectionName();
  return name == ".xdata" || name == ".pdata" || name.starts_with(".xdata$") ||
         name.starts_with(".pdata$");
}

DefinedImportData *getAddressedImport(Symbol *s) {
  if (auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s))
    return thunk->wrappedSym;
  // Data that resolved to its import is a copy of the import's symbol; the
  // import tables know the original.
  auto *data = dyn_cast_or_null<DefinedImportData>(s);
  if (!data || !data->isRuntimePseudoReloc)
    return nullptr;
  return cast<DefinedImportData>(data->file->impSym);
}

std::optional<int64_t> getAddressWordAddend(SectionChunk *sc,
                                            const coff_relocation &rel) {
  const Configuration &config = sc->file->symtab.ctx.config;
  if (uint64_t(rel.VirtualAddress) + config.wordsize > sc->getSize())
    return std::nullopt;
  const uint8_t *word = sc->getContents().data() + rel.VirtualAddress;
  return config.is64() ? int64_t(read64le(word)) : int32_t(read32le(word));
}

static bool isDelayLoaded(COFFLinkerContext &ctx, DefinedImportData *imp) {
  return ctx.config.delayLoads.contains(imp->getDLLName().lower());
}

// Whether rel, a reference to an import address table entry, reads the whole
// address the entry holds: a load that its object describes, an ARM64 adrp or
// ldr of it, or an entry of a KCFI type's list, which is the address of a word
// that the type's open routine reads whole.
static bool readsWholeEntry(SectionChunk *sc, const coff_relocation &rel,
                            bool code, bool arm64) {
  if (code && arm64)
    return isArm64PointerLoad(sc, rel);
  if (code) {
    std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
    return form && *form != LinkSiteAddress;
  }
  return isKCFIListEntry(sc, rel);
}

// What the scan of the relocations of the image's sections gathers for the
// decisions that follow it.
struct SlotScan {
  // A field that a relocation writes.
  struct Span {
    uint32_t offset;
    uint32_t size;
    const coff_relocation *rel;
    bool word;
  };
  // A section's candidate slots, and the pairs of its fields that overlap,
  // each field after the one before it that reaches furthest.
  struct Pending {
    SectionChunk *chunk;
    std::vector<ImportSlot> slots;
    Symbol *firstRef = nullptr;
    std::vector<std::pair<Span, Span>> overlaps;
  };
  // An instruction that is rewritten unless the import's thunk is its
  // address.
  struct Site {
    SectionChunk *chunk;
    uint32_t offset;
    DefinedImportData *imp;
  };
  // How the references other than its slots use an import's address: not at
  // all, only to read it whole, or otherwise.
  enum Use : uint8_t { Unused, Read, Other };

  void use(DefinedImportData *imp, Use u) {
    Use &v = uses[imp->file];
    v = std::max(v, u);
  }

  std::vector<Pending> pending;
  std::vector<Site> addressSites;
  // On ARM64 the loads of a delay-loaded function's entry are rewritten for
  // the symbol as a whole, since an adrp may serve several loads.
  MapVector<DefinedImportData *,
            std::vector<std::pair<SectionChunk *, uint32_t>>>
      delayLoads;
  DenseSet<DefinedImportData *> delayLoadsRead;
  // For each import thunk, the references to it, and those that a slot or a
  // rewritten instruction bypasses.
  MapVector<DefinedImportThunk *, std::pair<size_t, size_t>> thunkRefs;
  DenseMap<ImportFile *, Use> uses;
  // For each function whose address is a DLL's instance, the references that
  // keep the image's copy, such as calls.
  DenseMap<Symbol *, size_t> localFunctionRefs;
  // The symbols of the object being scanned that have been reported.
  SmallPtrSet<Symbol *, 4> reported;
  // The symbol table indices that the guard metadata of the object being
  // scanned lists as address-taken, once read.
  std::optional<DenseSet<uint32_t>> guardFids;
  // The fields of the section being checked.
  std::vector<Span> spans;
};

// Collects into overlaps the pairs of fields of sc that overlap, which may not
// cover a slot.
static void
findOverlaps(SlotScan &scan, SectionChunk *sc,
             std::vector<std::pair<SlotScan::Span, SlotScan::Span>> &overlaps) {
  uint32_t wordsize = sc->file->symtab.ctx.config.wordsize;
  std::vector<SlotScan::Span> &spans = scan.spans;
  spans.clear();
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (rel.Type == 0) // IMAGE_REL_*_ABSOLUTE writes nothing.
      continue;
    bool word = sc->isAddressWord(rel);
    spans.push_back({rel.VirtualAddress, word ? wordsize : 4u, &rel, word});
  }
  llvm::stable_sort(spans,
                    [](const SlotScan::Span &a, const SlotScan::Span &b) {
                      return a.offset < b.offset;
                    });
  const SlotScan::Span *last = nullptr;
  for (const SlotScan::Span &s : spans) {
    if (last && uint64_t(last->offset) + last->size > s.offset)
      overlaps.push_back({*last, s});
    if (!last ||
        uint64_t(s.offset) + s.size > uint64_t(last->offset) + last->size)
      last = &s;
  }
}

// Every word that an in-place import slot covers has that slot as its only
// writer: a relocation overlapping it, which would also be a second writer of
// a base relocation, is an error. An identical duplicate is one binding.
static bool validate(COFFLinkerContext &ctx, SlotScan::Pending &p) {
  auto isSlot = [&](const SlotScan::Span &s) {
    auto it = llvm::partition_point(
        p.slots, [&](const ImportSlot &x) { return x.offset < s.offset; });
    return s.word && it != p.slots.end() && it->offset == s.offset;
  };
  bool ok = true;
  for (auto &[last, s] : p.overlaps) {
    bool lastSlot = isSlot(last);
    if (!lastSlot && !isSlot(s))
      continue;
    if (last.offset == s.offset && last.size == s.size &&
        last.rel->Type == s.rel->Type &&
        last.rel->SymbolTableIndex == s.rel->SymbolTableIndex)
      continue;
    Err(ctx) << p.chunk->file << ": the address of an import at offset 0x"
             << Twine::utohexstr(lastSlot ? last.offset : s.offset) << " in "
             << p.chunk->getSectionName()
             << " overlaps the relocation at offset 0x"
             << Twine::utohexstr(lastSlot ? s.offset : last.offset);
    ok = false;
  }
  if (!ok)
    return false;
  p.slots.erase(llvm::unique(p.slots,
                             [](const ImportSlot &a, const ImportSlot &b) {
                               return a.offset == b.offset;
                             }),
                p.slots.end());
  return true;
}

// Under -import-slots, a word of static data that holds the address of an
// import is an in-place import slot: the loader writes the address there
// through an import descriptor whose address table is the run of such words
// the word belongs to, so the word needs no base relocation, and a function's
// address is the function's, not its import thunk's. Code that takes the
// address of an imported function in an instruction its object describes, or
// in an ARM64 adrp and add pair, is rewritten to load it from the import
// address table, so that code and static data agree. An object that may take
// it in an instruction it does not describe makes the image use the import
// thunk as the function's address everywhere, with a warning. A delay-loaded
// function keeps its thunk, and code that loads its address from the import
// address table is rewritten to take the thunk's. A reference to data that
// resolved to its import other than by such a word cannot reach the data, and
// is an error.
//
// One scan of the relocations gathers what each decision needs, as MinGW's
// runtime pseudo-relocations and ELF's relocation scan do; the decisions are
// taken once it has settled which functions keep their thunks as their
// addresses, on which every slot and rewritten instruction depends.
void ImportSlotContents::bind() {
  Configuration &config = ctx.config;
  if (!config.importSlots || ctx.hybridSymtab || isArm64EC(config.machine))
    return;
  llvm::TimeTraceScope timeScope("Import slots");

  // An export of data that resolved to its import would publish the address
  // of the import address table entry.
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym);
        imp && imp->isRuntimePseudoReloc)
      Err(ctx) << "cannot export " << ctx.symtab.printSymbol(imp)
               << ": it is imported from " << imp->getDLLName()
               << "; export a forwarder to it instead";

  // An import bound at run time has a cell in .sealed in place of an entry of
  // an import address table, which code reads unchanged, and no descriptor.
  // The bounds of a DLL's KCFI code range, which a KCFI thunk loads together,
  // take one 16-byte block, start then end.
  DenseMap<DefinedImportData *, DefinedImportData *> rangeEnds;
  DenseSet<DefinedImportData *> rangeEndSet;
  for (const KCFIState::ImportedRange &r : ctx.kcfi.importedRanges) {
    rangeEnds[r.start] = r.end;
    rangeEndSet.insert(r.end);
  }
  auto addCell = [&](DefinedImportData *imp, uint32_t align) {
    if (imp->getExternalName().empty()) {
      Err(ctx) << "cannot bind " << ctx.symtab.printSymbol(imp)
               << ", imported from " << imp->getDLLName()
               << " by ordinal, at run time";
      return;
    }
    RunTimeCell c{imp, make<RunTimeCellChunk>(ctx),
                  make<StringChunk>(imp->getExternalName()),
                  imp->file->isHostImport
                      ? nullptr
                      : make<WideStringChunk>(imp->getDLLName())};
    c.cell->setAlignment(std::max<uint32_t>(align, c.cell->getAlignment()));
    imp->setLocation(c.cell);
    // A .refptr pointer to the function reads the cell instead.
    StringRef name = imp->getName().drop_front(strlen("__imp_"));
    if (Symbol *s = ctx.symtab.find(name))
      ctx.symtab.replaceRefPtr(s, name, imp);
    runTimeNames.push_back(c.name);
    if (c.dllName)
      runTimeNames.push_back(c.dllName);
    runTimeCells.push_back(c);
  };
  for (ImportFile *f : ctx.importFileInstances) {
    DefinedImportData *imp = f->impSym;
    if (&f->symtab != &ctx.symtab || !f->live || !imp ||
        !f->isBoundAtRunTime() || rangeEndSet.contains(imp))
      continue;
    DefinedImportData *end = rangeEnds.lookup(imp);
    addCell(imp, end ? 16 : 1);
    if (end)
      addCell(end, 1);
  }

  if (!ctx.symtab.offeredFunctions.empty())
    keepUndescribedFunctionAddresses();

  SlotScan scan;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab)
      continue;
    scan.reported.clear();
    scan.guardFids.reset();
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (sc && sc->live &&
          !(sc->header->Characteristics & IMAGE_SCN_MEM_DISCARDABLE))
        scanChunk(scan, sc);
    }
  }
  resolve(scan);
}

// x86-64 cl.exe has no inline assembly, and MSVC's control-flow guard fails at
// run time if a function-typed address-take is missing from an object's
// .gfids$y table. So in an object built with /guard:cf, a REL32 against a
// function-typed symbol that the table does not list is a branch. A symbol
// declared as data, which keeps a function out of the table, proves nothing.
static bool isGuardMetadataBranch(std::optional<DenseSet<uint32_t>> &fids,
                                  ObjFile *file, const coff_relocation &rel) {
  if (!file->hasGuardCF())
    return false;
  Expected<COFFSymbolRef> sym =
      file->getCOFFObj()->getSymbol(rel.SymbolTableIndex);
  if (!sym) {
    consumeError(sym.takeError());
    return false;
  }
  if (sym->getComplexType() != COFF::IMAGE_SYM_DTYPE_FUNCTION)
    return false;
  if (!fids) {
    fids.emplace();
    for (SectionChunk *c : file->getGuardFidChunks()) {
      ArrayRef<uint8_t> data = c->getContents();
      for (size_t i = 0; i + 4 <= data.size(); i += 4)
        fids->insert(support::endian::read32le(data.data() + i));
    }
  }
  return !fids->contains(rel.SymbolTableIndex);
}

// A function that a DLL the image imports from offers, and whose address an
// object may take in an instruction it does not describe, keeps the image's
// copy as its address, which that instruction takes: the image does not split
// the function's address, as link.exe would not.
void ImportSlotContents::keepUndescribedFunctionAddresses() {
  MapVector<Symbol *, DefinedImportData *> &offered =
      ctx.symtab.offeredFunctions;
  bool arm64 = ctx.config.machine == ARM64;
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab || (!arm64 && file->describesSites()))
      continue;
    std::optional<DenseSet<uint32_t>> guardFids;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live ||
          !(sc->header->Characteristics & IMAGE_SCN_CNT_CODE))
        continue;
      for (const coff_relocation &rel : sc->getRelocs()) {
        if (rel.Type != (arm64 ? IMAGE_REL_ARM64_REL21 : IMAGE_REL_AMD64_REL32))
          continue;
        Symbol *s = file->getSymbol(rel.SymbolTableIndex);
        auto it = offered.find(s);
        if (it == offered.end() ||
            (!arm64 && isGuardMetadataBranch(guardFids, file, rel)))
          continue;
        Warn(ctx) << file << ": may take the address of "
                  << file->symtab.printSymbol(s) << ", which "
                  << it->second->getDLLName()
                  << " provides, in an instruction it does not describe, so "
                     "the image keeps its own copy as its address; rebuild "
                     "the object with clang";
        offered.erase(it);
      }
    }
  }
}

// A function that an object may take the address of in an instruction it
// does not describe, or in a 32-bit field of data, keeps its thunk as its
// address: its words in static data name the thunk, and the instructions that
// take its address are left as they are.
void ImportSlotContents::useThunkAsAddress(SlotScan &scan, ObjFile *file,
                                           Symbol *s, DefinedImportData *imp,
                                           SectionChunk *data) {
  imp->file->thunkIsAddress = true;
  if (!scan.reported.insert(s).second)
    return;
  if (data)
    Warn(ctx) << file << ": " << data->getSectionName()
              << " holds the address of " << file->symtab.printSymbol(s)
              << ", imported from " << imp->getDLLName()
              << ", in a 32-bit field, so the image uses its import thunk "
                 "as its address";
  else if (ctx.config.warnUndescribedAddress)
    Warn(ctx) << file << ": may take the address of "
              << file->symtab.printSymbol(s) << ", imported from "
              << imp->getDLLName()
              << ", in an instruction it does not describe, so the image "
                 "uses its import thunk as its address; declare it "
                 "imported, or rebuild the object with clang";
}

// A word that holds an address inside an import's data for which its DLL
// exports no name is the residual fill's to write. A read-only one moves with
// its chunk to pages that the fill makes read-only again once written, which a
// read-only section that may not move cannot give it. Thread-local data is the
// loader's to copy before any initializer runs.
void ImportSlotContents::addResidualWord(SectionChunk *sc, uint32_t offset,
                                         Symbol *s, DefinedImportData *imp,
                                         int64_t addend) {
  Configuration &config = ctx.config;
  StringRef name = getPartialSectionName(sc, config.mingw);
  bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
  StringRef why;
  if (name.starts_with(".tls")) {
    why = ", in thread-local data";
  } else if (readOnly && name != ".rdata") {
    why = ", in a read-only section that cannot move";
  } else if (config.machine == AMD64 || config.machine == ARM64) {
    if (readOnly)
      sealedChunks.insert(sc);
    residualWords.push_back({sc, offset, imp, addend, s});
    return;
  }
  Err(ctx) << sc->file << ": " << sc->getSectionName()
           << " holds the address of " << sc->file->symtab.printSymbol(s)
           << " plus " << addend << ", imported from " << imp->getDLLName()
           << ", which exports no name for it" << why;
}

void ImportSlotContents::scanChunk(SlotScan &scan, SectionChunk *sc) {
  Configuration &config = ctx.config;
  ObjFile *file = sc->file;
  bool arm64 = config.machine == ARM64;
  bool code = sc->header->Characteristics & IMAGE_SCN_CNT_CODE;
  ArrayRef<coff_relocation> relocs = sc->getRelocs();
  SlotScan::Pending p{sc, {}, nullptr, {}};
  for (size_t i = 0, e = relocs.size(); i != e; ++i) {
    const coff_relocation &rel = relocs[i];
    Symbol *s = file->getSymbol(rel.SymbolTableIndex);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(s);
    auto *data = dyn_cast_or_null<DefinedImportData>(s);
    if (thunk)
      ++scan.thunkRefs[thunk].first;

    // The address of a function that a DLL offers is the DLL's instance:
    // a described address-take loads it from the import's entry and a word of
    // static data takes it in place. Calls, and any other reference from
    // outside the function's own sections, keep the image's copy. The address
    // of a definition this DLL offers as an overridable default is the
    // program's choice, which the default's import holds, alike.
    DefinedImportData *offer = ctx.symtab.offeredFunctions.lookup(s);
    if (!offer)
      offer = ctx.symtab.preemptedOffers.lookup(s);
    if (offer) {
      if (code && arm64 && rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
          i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
        scan.addressSites.push_back({sc, rel.VirtualAddress, offer});
        scan.addressSites.push_back({sc, relocs[i + 1].VirtualAddress, offer});
        ++i;
        continue;
      }
      if (code && !arm64 && rel.Type == IMAGE_REL_AMD64_REL32 &&
          file->getLinkSiteForm(sc, rel.VirtualAddress) == LinkSiteAddress &&
          getDescribedSite(sc, rel) == LinkSiteAddress) {
        scan.addressSites.push_back({sc, rel.VirtualAddress, offer});
        continue;
      }
      if (!code && !isExceptionData(sc) && sc->isAddressWord(rel) &&
          getAddressWordAddend(sc, rel) == 0) {
        p.slots.push_back({sc, rel.VirtualAddress, offer});
        if (!p.firstRef)
          p.firstRef = s;
        continue;
      }
      SectionChunk *copy = cast<DefinedRegular>(s)->getChunk();
      if (sc != copy && llvm::none_of(copy->children(), [&](SectionChunk &c) {
            return &c == sc;
          }))
        ++scan.localFunctionRefs[s];
      continue;
    }

    // An import address table entry. The import's data itself is reached only
    // through its slots.
    if (data && !data->isRuntimePseudoReloc) {
      scan.use(data, readsWholeEntry(sc, rel, code, arm64) ? SlotScan::Read
                                                           : SlotScan::Other);
      // That of a delay-loaded function: a load of the address in code takes
      // the thunk's instead.
      if (!data->file->thunkSym || !isDelayLoaded(ctx, data))
        continue;
      if (!code)
        scan.delayLoadsRead.insert(data);
      else if (arm64 && isArm64PointerLoad(sc, rel))
        scan.delayLoads[data].push_back({sc, rel.VirtualAddress});
      else if (arm64)
        scan.delayLoadsRead.insert(data);
      else if (std::optional<LinkSiteForm> form = getDescribedSite(sc, rel);
               form == LinkSiteLoad || form == LinkSiteLoadREX2)
        scan.addressSites.push_back({sc, rel.VirtualAddress, data});
      continue;
    }

    DefinedImportData *imp = getAddressedImport(s);
    if (!imp)
      continue;
    // A section-relative offset names a section of this image.
    if (sc->isSectionRelative(rel)) {
      if (scan.reported.insert(s).second)
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName() << ", but "
                 << sc->getSectionName()
                 << " refers to it with relocation type "
                 << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                 << ", which names a section of this image";
      continue;
    }
    if (code) {
      if (!thunk) {
        // The address of data that a DLL's instance preempted, which a
        // described instruction takes, becomes a load of the import's entry,
        // as that of a function does.
        bool preempted = file->symtab.preemptedData.contains(s);
        if (preempted && arm64 && rel.Type == IMAGE_REL_ARM64_PAGEBASE_REL21 &&
            i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
          scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
          scan.addressSites.push_back({sc, relocs[i + 1].VirtualAddress, imp});
          ++i;
          continue;
        }
        if (preempted && !arm64 && rel.Type == IMAGE_REL_AMD64_REL32 &&
            file->getLinkSiteForm(sc, rel.VirtualAddress) == LinkSiteAddress &&
            getDescribedSite(sc, rel) == LinkSiteAddress) {
          scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
          continue;
        }
        if (!scan.reported.insert(s).second)
          continue;
        if (preempted)
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is provided by " << imp->getDLLName()
                   << ", but is referenced as if it were local; recompile "
                      "the object, or give the symbol hidden visibility";
        else
          Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                   << " is imported from " << imp->getDLLName()
                   << ", but the object was compiled as if it were "
                      "local; mark its declaration, or compile the object "
                      "with -fauto-import";
        continue;
      }
      if (sc->isAddressWord(rel)) {
        Err(ctx) << file << ": " << sc->getSectionName()
                 << " is executable and holds the address of "
                 << file->symtab.printSymbol(s) << ", imported from "
                 << imp->getDLLName();
        continue;
      }
      if (isDelayLoaded(ctx, imp))
        continue;
      if (arm64) {
        switch (rel.Type) {
        case IMAGE_REL_ARM64_PAGEBASE_REL21:
          if (i + 1 != e && isArm64AddressPair(sc, rel, relocs[i + 1])) {
            scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
            scan.addressSites.push_back(
                {sc, relocs[i + 1].VirtualAddress, imp});
            ++scan.thunkRefs[thunk].first;
            scan.thunkRefs[thunk].second += 2;
            ++i;
          } else {
            useThunkAsAddress(scan, file, s, imp);
          }
          break;
        case IMAGE_REL_ARM64_PAGEOFFSET_12A:
        case IMAGE_REL_ARM64_REL21:
          useThunkAsAddress(scan, file, s, imp);
          break;
        default:
          break;
        }
        continue;
      }
      switch (rel.Type) {
      case IMAGE_REL_AMD64_REL32: {
        if (!file->describesSites()) {
          if (!isGuardMetadataBranch(scan.guardFids, file, rel))
            useThunkAsAddress(scan, file, s, imp);
          break;
        }
        std::optional<LinkSiteForm> form =
            file->getLinkSiteForm(sc, rel.VirtualAddress);
        if (form == LinkSiteAddress) {
          if (getDescribedSite(sc, rel) == LinkSiteAddress) {
            scan.addressSites.push_back({sc, rel.VirtualAddress, imp});
            ++scan.thunkRefs[thunk].second;
          } else {
            Err(ctx) << file << ": the instruction at offset 0x"
                     << Twine::utohexstr(rel.VirtualAddress) << " in "
                     << sc->getSectionName()
                     << " is not the one its link-only record describes";
          }
        } else if (form == LinkSiteOther) {
          useThunkAsAddress(scan, file, s, imp);
        }
        break;
      }
      case IMAGE_REL_AMD64_REL32_1:
      case IMAGE_REL_AMD64_REL32_2:
      case IMAGE_REL_AMD64_REL32_3:
      case IMAGE_REL_AMD64_REL32_4:
      case IMAGE_REL_AMD64_REL32_5:
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName()
                 << ", but an instruction in " << sc->getSectionName()
                 << " reads its bytes, which its import thunk's are not";
        break;
      default:
        break;
      }
      continue;
    }

    // A 32-bit field cannot hold another image's address. A field through
    // which the function is only called, as a frame's handler is in unwind
    // data, is served by its import thunk; any other function address there
    // makes the thunk the function's address image-wide. An object that lists
    // its call-only fields says which they are; otherwise they are those of
    // exception data, which the PE format defines as RVAs that the system
    // calls.
    if (!sc->isAddressWord(rel)) {
      bool callOnly = file->listsCallOnly()
                          ? file->isCallOnlyRef(sc, rel.VirtualAddress)
                          : isExceptionData(sc);
      if (thunk && !callOnly)
        useThunkAsAddress(scan, file, s, imp, sc);
      else if (!thunk && scan.reported.insert(s).second)
        Err(ctx) << file << ": " << file->symtab.printSymbol(s)
                 << " is imported from " << imp->getDLLName() << ", but "
                 << sc->getSectionName()
                 << " refers to it with relocation type "
                 << file->getCOFFObj()->getRelocationTypeName(rel.Type)
                 << ", which cannot reach another image";
      continue;
    }
    if (isDelayLoaded(ctx, imp))
      continue;
    std::optional<int64_t> addend = getAddressWordAddend(sc, rel);
    if (!addend) {
      Err(ctx) << file << ": the address of " << file->symtab.printSymbol(s)
               << " at offset 0x" << Twine::utohexstr(rel.VirtualAddress)
               << " extends past the end of " << sc->getSectionName()
               << " (size 0x" << Twine::utohexstr(sc->getSize()) << ")";
      continue;
    }
    // The loader adds nothing to the address it writes, so a word that holds
    // an address inside the import's data takes the name that its DLL exports
    // for that address.
    if (*addend) {
      DefinedImportData *interior = ctx.symtab.findInteriorImport(imp, *addend);
      if (!interior) {
        if (thunk)
          Err(ctx) << file << ": " << sc->getSectionName()
                   << " holds the address of " << file->symtab.printSymbol(s)
                   << " plus " << *addend << ", imported from "
                   << imp->getDLLName()
                   << ", but no address inside a function is imported";
        else
          addResidualWord(sc, rel.VirtualAddress, s, imp, *addend);
        continue;
      }
      if (!interior->file->live)
        addImport(interior);
      interiorBases.try_emplace(imp->file, sc, rel.VirtualAddress);
      imp = interior;
    }
    p.slots.push_back({sc, rel.VirtualAddress, imp});
    if (thunk)
      ++scan.thunkRefs[thunk].second;
    if (!p.firstRef)
      p.firstRef = s;
  }
  if (p.slots.empty())
    return;
  findOverlaps(scan, sc, p.overlaps);
  scan.pending.push_back(std::move(p));
}

void ImportSlotContents::resolve(SlotScan &scan) {
  Configuration &config = ctx.config;
  heldBackChunks.insert(sealedChunks.begin(), sealedChunks.end());

  // The thunk whose address a rewritten load takes is kept in the image. An
  // instruction that takes an imported function's address loads it from the
  // import's entry once rewritten.
  auto keepThunk = [](DefinedImportData *imp) {
    cast<DefinedImportThunk>(imp->file->thunkSym)->getChunk()->live = true;
  };
  for (const SlotScan::Site &site : scan.addressSites) {
    if (site.imp->file->thunkIsAddress)
      continue;
    ctx.importSites.insert({site.chunk, site.offset});
    site.chunk->hasImportSites = true;
    if (isDelayLoaded(ctx, site.imp))
      keepThunk(site.imp);
    else
      scan.use(site.imp, SlotScan::Read);
  }
  for (auto &[imp, sites] : scan.delayLoads) {
    if (scan.delayLoadsRead.contains(imp))
      continue;
    for (auto [sc, offset] : sites) {
      ctx.importSites.insert({sc, offset});
      sc->hasImportSites = true;
    }
    keepThunk(imp);
  }

  // An import thunk that every reference bypasses is left out of the image,
  // and so out of the Control Flow Guard tables, unless something other than
  // a relocation names it.
  DenseSet<Symbol *> named(config.gcroot.begin(), config.gcroot.end());
  for (Export &e : ctx.symtab.exports)
    named.insert(e.sym);
  // The words of static data that hold the address of an import bound at run
  // time name its thunk until the fill writes them.
  for (auto &[thunk, refs] : scan.thunkRefs)
    if (refs.first == refs.second && !thunk->wrappedSym->file->thunkIsAddress &&
        !thunk->wrappedSym->file->isBoundAtRunTime() &&
        !isDelayLoaded(ctx, thunk->wrappedSym) && !named.contains(thunk))
      thunk->getChunk()->live = false;

  // A function's copy that no call keeps is discarded, as one that no
  // reference reached would have been; a root names the function, whose
  // address is the DLL's instance, not the copy.
  for (auto &[sym, imp] : ctx.symtab.offeredFunctions) {
    if (scan.localFunctionRefs.lookup(sym))
      continue;
    SectionChunk *copy = cast<DefinedRegular>(sym)->getChunk();
    copy->live = false;
    for (SectionChunk &child : copy->children())
      child.live = false;
  }

  std::vector<SectionChunk *> writable;
  for (SlotScan::Pending &p : scan.pending) {
    SectionChunk *sc = p.chunk;
    // A word holding the address of an import bound at run time is the fill's
    // to write, from the import's cell.
    llvm::erase_if(p.slots, [&](const ImportSlot &s) {
      if (!s.sym->file->isBoundAtRunTime() || s.sym->file->thunkIsAddress)
        return false;
      addResidualWord(sc, s.offset, s.sym, s.sym, 0);
      return true;
    });
    llvm::erase_if(p.slots, [](const ImportSlot &s) {
      return s.sym->file->thunkIsAddress;
    });
    if (p.slots.empty())
      continue;
    llvm::stable_sort(p.slots, [](const ImportSlot &a, const ImportSlot &b) {
      return a.offset < b.offset;
    });
    if (!validate(ctx, p))
      continue;
    // A read-only slot can be written only inside the range the loader
    // makes writable while it binds imports. A chunk of .rdata is moved
    // there. A read-only section whose order or bounds its program may rely
    // on keeps its place: another section is laid out beside that range, and
    // the $-groups of .rdata, together and in their order, at its start.
    // Exception data never holds an import's address.
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE);
    StringRef name = getPartialSectionName(sc, config.mingw);
    StringRef outName = getOutputSectionName(name, config.mingw);
    if (readOnly && outName == ".xdata") {
      Err(ctx) << sc->file << ": " << sc->getSectionName()
               << " is read-only and holds the address of "
               << sc->file->symtab.printSymbol(p.firstRef) << ", imported from "
               << p.slots[0].sym->getDLLName()
               << ", but cannot be laid out with the import address table";
      continue;
    }
    sc->hasImportSlots = true;
    ctx.importSlots[sc] = std::move(p.slots);
    // A chunk that also holds residual words is writable until they are
    // written.
    if (sealedChunks.contains(sc)) {
      writable.push_back(sc);
      continue;
    }
    if (readOnly && name == ".rdata") {
      slotChunks.push_back(sc);
      continue;
    }
    if (!readOnly && name == ".data" &&
        sc->getOutputCharacteristics() ==
            (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
             IMAGE_SCN_MEM_WRITE)) {
      writableSlotChunks.push_back(sc);
      continue;
    }
    if (readOnly && outName == ".rdata")
      slotRdataGroups = true;
    else if (readOnly)
      slotSections.insert(outName);
    writable.push_back(sc);
  }
  // How the references other than slots use each import: an export or a root
  // uses it otherwise, as KCFI's lists may, and the residual fill and a live
  // import thunk read it.
  SmallVector<DefinedImportData *, 0> kcfiRead, kcfiOther;
  getKCFIImportUses(ctx, kcfiRead, kcfiOther);
  for (DefinedImportData *imp : kcfiRead)
    scan.use(imp, SlotScan::Read);
  for (DefinedImportData *imp : kcfiOther)
    scan.use(imp, SlotScan::Other);
  for (Export &e : ctx.symtab.exports)
    if (auto *imp = dyn_cast_or_null<DefinedImportData>(e.sym))
      scan.use(imp, SlotScan::Other);
  for (Symbol *s : config.gcroot)
    if (auto *imp = dyn_cast<DefinedImportData>(s))
      scan.use(imp, SlotScan::Other);
  for (const ResidualWord &w : residualWords)
    scan.use(w.imp, SlotScan::Read);
  auto getUse = [&](ImportFile *file) {
    SlotScan::Use u = scan.uses.lookup(file);
    auto *thunk = dyn_cast_or_null<DefinedImportThunk>(file->thunkSym);
    return thunk && thunk->getChunk()->live ? std::max(u, SlotScan::Read) : u;
  };
  // An import that only the discarded sections of preempted data used needs
  // no entry.
  auto omitPreemptedGroupImports =
      [&](const DenseSet<ImportFile *> &slotted,
          DenseSet<DefinedImportData *> &unneeded) {
        for (ImportFile *file : ctx.symtab.preemptedGroupImports) {
          if (!file->live || getUse(file) != SlotScan::Unused ||
              slotted.contains(file) || interiorBases.contains(file))
            continue;
          file->live = false;
          unneeded.insert(file->impSym);
        }
      };
  if (ctx.importSlots.empty()) {
    DenseSet<DefinedImportData *> unneeded;
    omitPreemptedGroupImports({}, unneeded);
    llvm::erase_if(idata.imports, [&](DefinedImportData *imp) {
      return unneeded.contains(imp);
    });
    return;
  }

  // The slot chunks laid out together are ordered by the DLL of their first
  // slot, so that single-pointer chunks of one DLL form one run.
  auto dllIndex = [&](SectionChunk *sc) {
    StringRef dll = ctx.importSlots[sc].front().sym->getDLLName();
    return config.dllOrder[dll.lower()];
  };
  for (std::vector<SectionChunk *> *v : {&writableSlotChunks, &slotChunks}) {
    llvm::stable_sort(*v, [&](SectionChunk *a, SectionChunk *b) {
      return dllIndex(a) < dllIndex(b);
    });
    heldBackChunks.insert(v->begin(), v->end());
  }

  // Slots of one DLL one word apart form a run: within a chunk, and across
  // two chunks laid out together with no padding between them.
  std::vector<ImportSlot *> *run = nullptr;
  ImportSlot *prev = nullptr;
  auto follows = [&](ImportSlot &s) {
    if (!prev ||
        !prev->sym->getDLLName().equals_insensitive(s.sym->getDLLName()))
      return false;
    if (prev->chunk == s.chunk)
      return s.offset == prev->offset + config.wordsize;
    SectionChunk *a = prev->chunk;
    return heldBackChunks.contains(a) && heldBackChunks.contains(s.chunk) &&
           !ctx.chunkPins.contains(s.chunk) && s.offset == 0 &&
           prev->offset + config.wordsize == a->getSize() &&
           s.chunk->getAlignment() <= a->getAlignment() &&
           a->getSize() % s.chunk->getAlignment() == 0;
  };
  auto addRuns = [&](SectionChunk *sc) {
    for (ImportSlot &s : ctx.importSlots[sc]) {
      if (!follows(s)) {
        idata.slotRuns.emplace_back();
        run = &idata.slotRuns.back();
      }
      run->push_back(&s);
      prev = &s;
    }
  };
  for (SectionChunk *sc : writable) {
    prev = nullptr;
    addRuns(sc);
  }
  for (std::vector<SectionChunk *> *v : {&writableSlotChunks, &slotChunks}) {
    prev = nullptr;
    for (SectionChunk *sc : *v)
      addRuns(sc);
  }

  // An import whose address the image keeps in in-place slots needs no entry of
  // its own in its DLL's import address table when nothing else needs one. Its
  // address is then taken to be one of its slots, which every other user reads
  // instead: code that loads the whole address through an instruction its
  // object describes, or through ARM64 adrp and ldr, the import thunk, and the
  // lists of KCFI's open routines. Such a slot must be naturally aligned and in
  // a read-only section that no /merge or /section option changes, so that the
  // image cannot write what they read. An import that anything uses otherwise
  // keeps its entry: a reference from data, an instruction that takes the
  // entry's address or reads part of it, an object that does not describe its
  // instructions, an export or a root.
  DenseSet<ImportFile *> slotted;
  for (const std::vector<ImportSlot *> &slotRun : idata.slotRuns)
    for (ImportSlot *slot : slotRun)
      slotted.insert(slot->sym->file);

  // An import whose words all take interior names, and that nothing else
  // uses, needs no entry at all. What still names it, such as debug
  // information, is given the place of its first such word.
  DenseSet<DefinedImportData *> unneeded;
  for (auto &[file, word] : interiorBases) {
    if (getUse(file) != SlotScan::Unused || slotted.contains(file))
      continue;
    file->live = false;
    file->impSym->setLocation(word.first, word.second);
    unneeded.insert(file->impSym);
  }
  omitPreemptedGroupImports(slotted, unneeded);
  llvm::erase_if(idata.imports, [&](DefinedImportData *imp) {
    return unneeded.contains(imp);
  });

  // A read-only chunk with residual words is writable until they are written.
  auto readable = [&](const ImportSlot &s) {
    StringRef outName = getOutputSectionName(
        getPartialSectionName(s.chunk, config.mingw), config.mingw);
    return !(s.chunk->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
           !sealedChunks.contains(s.chunk) &&
           s.chunk->getAlignment() >= config.wordsize &&
           s.offset % config.wordsize == 0 && !config.merge.count(outName) &&
           !config.section.count(outName);
  };
  // The first readable slot of each import in the order of the runs, and
  // for an import that nothing else uses, its first slot if none is.
  MapVector<ImportFile *, ImportSlot *> chosen;
  for (bool any : {false, true})
    for (const std::vector<ImportSlot *> &slotRun : idata.slotRuns)
      for (ImportSlot *slot : slotRun) {
        SlotScan::Use u = getUse(slot->sym->file);
        if (u != SlotScan::Other &&
            (any ? u == SlotScan::Unused : readable(*slot)))
          chosen.try_emplace(slot->sym->file, slot);
      }
  for (auto &[file, slot] : chosen) {
    slot->sym->setLocation(slot->chunk, slot->offset);
    idata.slotOnly.insert(slot->sym);
    if (getUse(file) == SlotScan::Read)
      slotReadImports.push_back(slot->sym);
  }
}

// Adds an import that nothing else made live to the import tables, for a pass
// after createImportTables that needs it.
void ImportSlotContents::addImport(DefinedImportData *imp) {
  imp->file->live = true;
  ctx.config.dllOrder.try_emplace(StringRef(imp->file->dllName).lower(),
                                  ctx.config.dllOrder.size());
  idata.add(imp);
}

// Under -import-slots, the residual fill writes the words of static data that
// hold an address inside an import for which its DLL exports no name, as the
// loader would write the import's address. It runs from the image's C
// initializer table, ahead of every initializer, and makes the pages of the
// read-only words read-only once it has written them.
void ImportSlotContents::createResidualFill() {
  if (residualWords.empty() && runTimeCells.empty())
    return;
  Configuration &config = ctx.config;
  auto fail = [&](const Twine &why) {
    if (!runTimeCells.empty()) {
      DefinedImportData *imp = runTimeCells.front().imp;
      Err(ctx) << "cannot bind " << ctx.symtab.printSymbol(imp)
               << ", imported from " << imp->getDLLName()
               << ", at run time: " << why;
      return;
    }
    const ResidualWord &first = residualWords.front();
    Err(ctx) << first.chunk->file << ": " << first.chunk->getSectionName()
             << " holds the address of "
             << first.chunk->file->symtab.printSymbol(first.sym) << " plus "
             << first.addend << ", imported from " << first.imp->getDLLName()
             << ", which exports no name for it, and " << why;
  };
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return fail("the image has no C initializer table (__xi_a) to write it");

  // The fill binds cells through ntdll's resolver, which follows forwarders
  // and grants an export-suppressed target as GetProcAddress does.
  auto getNtdll = [&](StringRef name) -> DefinedImportData * {
    auto *imp = dyn_cast_or_null<DefinedImportData>(
        ctx.symtab.find(("__imp_" + name).str()));
    if (!imp) {
      fail("the image has no " + name + " from ntdll.lib");
      return nullptr;
    }
    if (config.delayLoads.contains(imp->getDLLName().lower())) {
      fail(name + " is delay-loaded");
      return nullptr;
    }
    if (!imp->file->live)
      addImport(imp);
    return imp;
  };
  DefinedImportData *getProc = nullptr, *getDll = nullptr;
  if (!runTimeCells.empty()) {
    getProc = getNtdll("LdrGetProcedureAddressForCaller");
    if (!getProc)
      return;
    if (llvm::any_of(
            runTimeCells,
            [](const RunTimeCell &c) { return c.dllName != nullptr; }) &&
        !(getDll = getNtdll("LdrGetDllHandleEx")))
      return;
  }

  DefinedImportData *protect = nullptr;
  if (!sealedChunks.empty() || !runTimeCells.empty()) {
    if (config.align % 4096)
      return fail("its section alignment is smaller than a page, which the "
                  "image cannot make read-only once written");
    protect = dyn_cast_or_null<DefinedImportData>(
        ctx.symtab.find("__imp_NtProtectVirtualMemory"));
    if (!protect)
      return fail("the image cannot make it read-only once written without "
                  "NtProtectVirtualMemory from ntdll.lib");
    if (config.delayLoads.contains(protect->getDLLName().lower()))
      return fail("NtProtectVirtualMemory, which makes it read-only once "
                  "written, is delay-loaded");
    if (!protect->file->live)
      addImport(protect);
  }

  Chunk *sealed = nullptr;
  if (!runTimeCells.empty())
    sealed = runTimeCells.front().cell;
  else if (!sealedChunks.empty())
    sealed = sealedChunks.front();
  auto *fill = make<ResidualFillChunk>(ctx, residualWords, runTimeCells, sealed,
                                       protect, getProc, getDll);
  residualFill = fill;
  residualFillPointer = make<LocalImportChunk>(
      ctx, make<DefinedSynthetic>("__llvm_residual_fill", fill));
  if (protect) {
    auto *unwind = make<ResidualFillUnwindChunk>(ctx, fill);
    residualFillUnwind = unwind;
    residualFillPdata = make<ResidualFillPdataChunk>(ctx, fill, unwind);
  }
}

// The TLS directory's template start and index: the targets of the
// relocations of its first and third fields. Either is null when the image has
// no directory or the directory names none.
static TLSTemplate findTLSTemplate(COFFLinkerContext &ctx) {
  TLSTemplate tls;
  auto *dir =
      dyn_cast_or_null<DefinedRegular>(ctx.symtab.findUnderscore("_tls_used"));
  if (!dir)
    return tls;
  SectionChunk *sc = dir->getChunk();
  for (const coff_relocation &rel : sc->getRelocs()) {
    if (!sc->isAddressWord(rel))
      continue;
    uint64_t field = rel.VirtualAddress - dir->getValue();
    auto *target =
        dyn_cast_or_null<Defined>(sc->file->getSymbol(rel.SymbolTableIndex));
    int64_t addend = getAddressWordAddend(sc, rel).value_or(0);
    if (field == 0) {
      tls.start = target;
      tls.startAddend = addend;
    } else if (field == 2 * ctx.config.wordsize) {
      tls.index = target;
      tls.indexAddend = addend;
    }
  }
  return tls;
}

// Under -import-slots, an executable's in-place import slots in its
// thread-local template are bound in the template only after the loader has
// copied it into the main thread's block; the copy writes them there too.
void ImportSlotContents::createThreadLocalSlotCopy() {
  Configuration &config = ctx.config;
  if (!config.importSlots || config.dll || ctx.hybridSymtab ||
      (config.machine != AMD64 && config.machine != ARM64))
    return;
  std::vector<const ImportSlot *> tlsSlots;
  for (auto &[sc, slots] : ctx.importSlots)
    if (getPartialSectionName(const_cast<SectionChunk *>(sc), config.mingw)
            .starts_with(".tls"))
      for (const ImportSlot &slot : slots)
        tlsSlots.push_back(&slot);
  // Without a TLS directory the loader gives no thread a block.
  if (tlsSlots.empty() || !ctx.symtab.findUnderscore("_tls_used"))
    return;
  auto fail = [&](const Twine &why) {
    Err(ctx) << tlsSlots.front()->chunk->file
             << ": the address of an import in thread-local data needs a copy "
                "into the main thread's block, but "
             << why;
  };
  TLSTemplate tls = findTLSTemplate(ctx);
  if (!tls.start || !tls.index)
    return fail("the image's TLS directory (_tls_used) names no template or "
                "index");
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xl_a")))
    return fail("the image has no TLS callback array (__xl_a) to run it");
  auto *copy = make<ThreadLocalSlotCopyChunk>(ctx, std::move(tlsSlots), tls);
  threadLocalSlotCopy = copy;
  threadLocalSlotCopyPointer = make<LocalImportChunk>(
      ctx, make<DefinedSynthetic>("__llvm_thread_local_slot_copy", copy));
}

// Under -import-slots, a DLL that imports from an executable may call it from
// its initializers while the DLLs initialize, before the executable's TLS
// callbacks run the copy of its thread-local slots; the DLL runs the copy the
// executable names first. Without a C initializer table nothing would run it,
// and the DLL is left as it was.
void ImportSlotContents::createThreadLocalSlotCopyCall() {
  Configuration &config = ctx.config;
  if (!config.importSlots || !config.dll || ctx.hybridSymtab ||
      (config.machine != AMD64 && config.machine != ARM64))
    return;
  if (llvm::none_of(ctx.importFileInstances, [](ImportFile *f) {
        return f->live && StringRef(f->dllName).ends_with_insensitive(".exe");
      }))
    return;
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return;
  // The guard's pointer, where the image has it: the dispatch on x86-64, the
  // check on AArch64.
  auto *guard = dyn_cast_or_null<Defined>(
      ctx.symtab.find(config.machine == AMD64 ? "__guard_dispatch_icall_fptr"
                                              : "__guard_check_icall_fptr"));
  if (auto *d = dyn_cast_or_null<DefinedRegular>(guard); d && !d->isLive())
    guard = nullptr;
  auto *call = make<ThreadLocalSlotCopyCallChunk>(ctx, guard);
  threadLocalSlotCopyCall = call;
  threadLocalSlotCopyCallPointer = make<LocalImportChunk>(
      ctx, make<DefinedSynthetic>("__llvm_thread_local_slot_copy_call", call));
  if (config.machine == ARM64) {
    auto *unwind = make<ResidualFillUnwindChunk>(ctx, call);
    threadLocalSlotCopyCallUnwind = unwind;
    threadLocalSlotCopyCallPdata =
        make<ResidualFillPdataChunk>(ctx, call, unwind);
  }
}

// Under -import-slots, a DLL's cells of the overridable defaults it imports
// are the words through which it reaches them: each in-place import slot of a
// default, and the default's own entry, which its calls and the instructions
// that take its address read. The fill writes those of a replaced default with
// the replacement, so the DLL observes and calls the one the program chose, as
// on ELF. An executable binds its own at link time, from the table it writes.
void ImportSlotContents::createOverrideCellFill() {
  Configuration &config = ctx.config;
  if (!config.importSlots || !config.dll || ctx.hybridSymtab ||
      (config.machine != AMD64 && config.machine != ARM64))
    return;
  std::vector<OverrideCell> cells;
  ctx.symtab.readOverrideRecords();
  auto add = [&](SectionChunk *sc, uint32_t offset, DefinedImportData *imp,
                 bool readOnly, bool threadLocal) {
    StringRef name = imp->getName().drop_front(strlen("__imp_"));
    if (std::optional<uint32_t> k = ctx.symtab.overrideImportSlot(name))
      cells.push_back({sc, offset, imp, *k, 0, readOnly, threadLocal});
    else if (ctx.symtab.defaultsByName.contains(name))
      cells.push_back(
          {sc, offset, imp, 0, overrideNameKey(name), readOnly, threadLocal});
  };
  // A slot in a chunk with residual words is in a section that stays writable
  // until the residual fill, which runs after this one, seals it.
  for (auto &[sc, slots] : ctx.importSlots) {
    auto *chunk = const_cast<SectionChunk *>(sc);
    bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
                    !sealedChunks.contains(chunk);
    bool threadLocal =
        getPartialSectionName(chunk, config.mingw).starts_with(".tls");
    for (const ImportSlot &slot : slots)
      add(chunk, slot.offset, slot.sym, readOnly, threadLocal);
  }
  // An import whose entry is one of its slots is covered by that slot.
  for (DefinedImportData *imp : idata.imports)
    if (!idata.slotOnly.contains(imp))
      add(nullptr, 0, imp, true, false);
  // The DLL's own defaults have cells too, as on ELF its address in the
  // defining object is the program's one: the pointer its address-takes read,
  // and the words of its static data that hold its address. Each holds the
  // default until the fill writes the replacement. Both are in read-only data
  // beside the import address tables, or in writable data. A default with a
  // slot is keyed by the slot its definition records, and one without by its
  // name.
  auto addOwn = [&](StringRef name, Chunk *c, uint32_t offset, bool readOnly,
                    bool threadLocal) {
    if (ctx.symtab.isOwnSlotDefault(name))
      cells.push_back({c, offset, nullptr, *ctx.symtab.overrideImportSlot(name),
                       0, readOnly, threadLocal});
    else if (ctx.symtab.overridablesByName.contains(name))
      cells.push_back({c, offset, nullptr, 0, overrideNameKey(name), readOnly,
                       threadLocal});
  };
  for (Chunk *c : ctx.symtab.localImportChunks) {
    auto *li = cast<LocalImportChunk>(c);
    if (!li->overrideDefault.empty())
      addOwn(li->overrideDefault, li, 0, true, false);
  }
  for (ObjFile *file : ctx.objFileInstances) {
    if (&file->symtab != &ctx.symtab)
      continue;
    for (Chunk *c : file->getChunks()) {
      auto *sc = dyn_cast_or_null<SectionChunk>(c);
      if (!sc || !sc->live ||
          (sc->header->Characteristics &
           (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_DISCARDABLE)) ||
          isExceptionData(sc))
        continue;
      bool readOnly = !(sc->header->Characteristics & IMAGE_SCN_MEM_WRITE) &&
                      !sealedChunks.contains(sc);
      bool threadLocal =
          getPartialSectionName(sc, config.mingw).starts_with(".tls");
      for (const coff_relocation &rel : sc->getRelocs())
        if (auto *d = dyn_cast_or_null<Defined>(
                file->getSymbol(rel.SymbolTableIndex));
            d && sc->isAddressWord(rel) && getAddressWordAddend(sc, rel) == 0)
          addOwn(d->getName(), sc, rel.VirtualAddress, readOnly, threadLocal);
    }
  }
  if (cells.empty())
    return;

  const OverrideCell &first = cells.front();
  auto fail = [&](const Twine &why) {
    if (first.imp)
      Err(ctx) << "the image reaches " << ctx.symtab.printSymbol(first.imp)
               << ", an overridable default imported from "
               << first.imp->getDLLName() << ", through a cell " << why;
    else
      Err(ctx) << "the image reaches its own overridable default through a "
                  "cell "
               << why;
  };
  // The anchor's mask, which the fill reads, has a bit for each of the first
  // 32 slots.
  for (const OverrideCell &c : cells)
    if (!c.key && c.slot >= 32)
      return fail("that the override table's mask cannot name");
  if (!isa_and_nonnull<Defined>(ctx.symtab.findUnderscore("__xi_a")))
    return fail("that the image has no C initializer table (__xi_a) to fill");
  // A thread-local cell is written in the template and in the loading thread's
  // block.
  TLSTemplate tls;
  if (llvm::any_of(cells,
                   [](const OverrideCell &c) { return c.threadLocal; })) {
    tls = findTLSTemplate(ctx);
    if (!tls.start || !tls.index)
      return fail("in thread-local data, but the image's TLS directory "
                  "(_tls_used) names no template or index");
  }
  DefinedImportData *protect = nullptr;
  if (llvm::any_of(cells, [](const OverrideCell &c) { return c.readOnly; })) {
    protect = dyn_cast_or_null<DefinedImportData>(
        ctx.symtab.find("__imp_NtProtectVirtualMemory"));
    if (!protect)
      return fail("that it cannot write without NtProtectVirtualMemory from "
                  "ntdll.lib");
    if (config.delayLoads.contains(protect->getDLLName().lower()))
      return fail("that it cannot write while NtProtectVirtualMemory is "
                  "delay-loaded");
    if (!protect->file->live)
      addImport(protect);
  }

  std::stable_partition(cells.begin(), cells.end(),
                        [](const OverrideCell &c) { return !c.readOnly; });
  auto *fill = make<OverrideCellFillChunk>(ctx, std::move(cells), protect, tls);
  overrideCellFill = fill;
  overrideCellFillPointer = make<LocalImportChunk>(
      ctx, make<DefinedSynthetic>("__llvm_override_cell_fill", fill));
  if (protect) {
    auto *unwind = make<ResidualFillUnwindChunk>(ctx, fill);
    overrideCellFillUnwind = unwind;
    overrideCellFillPdata = make<ResidualFillPdataChunk>(ctx, fill, unwind);
  }
}

// The loader writes an in-place import slot with an absolute address, makes
// only the import address table directory writable while it binds imports, and
// restores one protection over all of it. Each slot's word must therefore be
// in a section that is not executable, a read-only one inside the directory,
// whose page-rounded range has one protection; each run must still be one word
// a slot.
void ImportSlotContents::check(Chunk *iatStart, uint64_t iatSize) {
  if (ctx.importSlots.empty())
    return;
  const uint32_t perms =
      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;
  uint64_t iatBegin = iatStart ? iatStart->getRVA() : 0;
  uint64_t iatLimit = iatStart ? iatBegin + iatSize : 0;
  for (auto &[sc, slots] : ctx.importSlots) {
    OutputSection *os = ctx.getOutputSection(sc);
    uint32_t chars = os->header.Characteristics;
    for (const ImportSlot &s : slots) {
      uint64_t rva = sc->getRVA() + s.offset;
      if (chars & IMAGE_SCN_MEM_EXECUTE)
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in executable section " << os->name;
      else if (!(chars & IMAGE_SCN_MEM_WRITE) &&
               (rva < iatBegin || rva + ctx.config.wordsize > iatLimit))
        Err(ctx) << sc->file << ": the address of an import at offset 0x"
                 << Twine::utohexstr(s.offset) << " in " << sc->getSectionName()
                 << " is in read-only section " << os->name
                 << " outside the import address table";
    }
  }
  uint64_t pageBegin = alignDown(iatBegin, ctx.config.align);
  uint64_t pageLimit = alignTo(iatLimit, ctx.config.align);
  for (OutputSection *os : ctx.outputSections) {
    if (os->getRVA() >= pageLimit ||
        os->getRVA() + os->getVirtualSize() <= pageBegin)
      continue;
    if ((os->header.Characteristics & perms) != IMAGE_SCN_MEM_READ)
      Err(ctx) << "section " << os->name
               << " shares a page with the import address table, which holds "
                  "in-place import slots, but is not read-only data";
  }
  for (const std::vector<ImportSlot *> &run : idata.slotRuns)
    for (size_t i = 1; i < run.size(); ++i)
      if (run[i]->chunk->getRVA() + run[i]->offset !=
          run[i - 1]->chunk->getRVA() + run[i - 1]->offset +
              ctx.config.wordsize)
        Err(ctx) << run[i]->chunk->file
                 << ": a run of in-place import slots in "
                 << run[i]->chunk->getSectionName()
                 << " is not contiguous in the image";
  for (DefinedImportData *imp : slotReadImports) {
    OutputSection *os = ctx.getOutputSection(imp->getChunk());
    if (imp->getRVA() % ctx.config.wordsize ||
        (os->header.Characteristics & IMAGE_SCN_MEM_WRITE))
      Err(ctx) << "code reads the address of " << ctx.symtab.printSymbol(imp)
               << " from an in-place import slot in " << os->name
               << " that is misaligned or writable";
  }
}
} // namespace lld::coff
