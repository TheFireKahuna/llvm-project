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

// Whether the bytes around a REL32 field at off are those of the form an
// object describes there. They verify the description, which is the only
// means of finding the instruction.
bool isSiteForm(ArrayRef<uint8_t> data, uint32_t off,
                llvm::COFF::LinkSiteForm form);

// The form that the object of sc describes for the instruction whose field
// rel is, if its bytes are that form's; none for a site of no form.
std::optional<llvm::COFF::LinkSiteForm>
getDescribedSite(const SectionChunk *sc,
                 const llvm::object::coff_relocation &rel);

// Whether adrp and add, ARM64 relocations in sc against one symbol, are an
// adrp and the add after it computing the symbol's address with no addend, as
// lld/ELF requires to relax such a pair.
bool isArm64AddressPair(const SectionChunk *sc,
                        const llvm::object::coff_relocation &adrp,
                        const llvm::object::coff_relocation &add);

// Whether rel is the adrp, or a 64-bit ldr with no offset, of a load of a
// pointer.
bool isArm64PointerLoad(const SectionChunk *sc,
                        const llvm::object::coff_relocation &rel);

// Whether the relocation at index i of sc, a code section, is the field of a
// load of a pointer, of the form given, that the object describes as one whose
// register only reads of memory read, so that the load may read another
// pointer to equal data. On ARM64, where the load is an ldr, sets page to the
// index of the relocation of the adrp distance instructions before it that
// gives the ldr the pointer's page; otherwise to i.
bool isValueOnlyLoad(const SectionChunk *sc, size_t i,
                     llvm::COFF::LinkSiteForm form, uint16_t distance,
                     size_t &page);

// Whether the references of file to local import pointers are reported only
// once local imports are bound, and only if they still read the pointer.
bool defersLocalImportWarning(const ObjFile *file, bool arm64);

// Binds each .refptr.X pointer to a local import pointer to X when X is in
// the image, or to zero when a weak X is absent, whose references
// bindLocalImports rewrites to reach X, or zero, directly.
void bindPointerCells(SymbolTable &symtab);

// Decides, after mark-live, which local import pointers a reference still
// reads.
void bindLocalImports(SymbolTable &symtab);

// Decides, once local imports are bound, which fused calls go through their
// register, which fused accesses and adds reach their data directly and which
// fused loads nothing reads, and makes the edits of each.
void bindFusedCalls(SymbolTable &symtab);

// Writes, into the contents of sc at buf, an ARM64 chunk, each fused access
// and add that bindFusedCalls decided reaches the data directly, where the
// offsets the data's address gives them fit their fields.
void writeFusedArm64(const SectionChunk *sc, uint8_t *buf);
} // namespace lld::coff

#endif
