//===- LocalImports.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_LOCALIMPORTS_H
#define LLD_COFF_LOCALIMPORTS_H

#include "lld/Common/LLVM.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace lld::coff {
class ObjFile;
class SectionChunk;
class SymbolTable;

// Whether the bytes around the REL32 field at off match the form an object
// describes there, verifying the description that locates the instruction.
bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off,
                llvm::COFF::LinkSiteForm form);

// The form the object of sc describes for rel's instruction, if its bytes
// match; none for a site of no form.
std::optional<llvm::COFF::LinkSiteForm>
getDescribedSite(const SectionChunk *sc,
                 const llvm::object::coff_relocation &rel);

// Whether adrp and add (ARM64 relocations in sc against one symbol) compute
// the symbol's address with no addend, the pair lld/ELF can relax.
bool isArm64AddressPair(const SectionChunk *sc,
                        const llvm::object::coff_relocation &adrp,
                        const llvm::object::coff_relocation &add);

// Whether rel is the adrp, or a 64-bit ldr with no offset, of a load of a
// pointer.
bool isArm64PointerLoad(const SectionChunk *sc,
                        const llvm::object::coff_relocation &rel);

// Whether the relocation at index i of sc, a code section, is the field of a
// pointer load of the given form whose register only memory reads read, so the
// load may read another pointer to equal data. On ARM64 (an ldr) sets page to
// the adrp relocation distance instructions before it; otherwise to i.
bool isValueOnlyLoad(const SectionChunk *sc, size_t i,
                     llvm::COFF::LinkSiteForm form, uint16_t distance,
                     size_t &page);

// Whether the references of file to local import pointers are reported only
// once local imports are bound, and only if they still read the pointer.
bool defersLocalImportWarning(const ObjFile *file, bool arm64);

// Binds each .refptr.X pointer to a local import to X (in the image) or to
// zero (absent weak X), whose references bindLocalImports rewrites directly.
void bindPointerCells(SymbolTable &symtab);

// Decides, after mark-live, which local import pointers a reference still
// reads.
void bindLocalImports(SymbolTable &symtab);

// Decides, once local imports are bound, which fused calls go through their
// register, which accesses and adds reach data directly, and which loads are
// dead; makes the edits of each.
void bindFusedCalls(SymbolTable &symtab);

// Writes into the ARM64 chunk sc's contents at buf the fused accesses and adds
// bindFusedCalls decided reach the data directly, where the data-address
// offsets fit their fields.
void writeFusedArm64(const SectionChunk *sc, uint8_t *buf);
} // namespace lld::coff

#endif
