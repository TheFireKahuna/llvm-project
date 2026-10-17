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
class Defined;
class DefinedLocalImport;
class ObjFile;
class SectionChunk;
class SymbolTable;
struct RelaxEdit;

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

// Rewrites the instruction at off in sc holding rel, a reference to the local
// import pointer li, to reach li's symbol directly where the instruction may,
// as bindLocalImports decided. Sets sym and type to the symbol and the
// relocation type that the rewritten instruction takes, and rewrite to the
// x86-64 form that rewriteLocalImportSite finishes once the symbol's address
// is known. Returns true if the instruction now needs no relocation.
bool relaxLocalImport(const SectionChunk *sc,
                      const llvm::object::coff_relocation &rel, uint8_t *off,
                      DefinedLocalImport *li, Defined *&sym, uint16_t &type,
                      std::optional<llvm::COFF::LinkSiteForm> &rewrite);

// Decides, once local imports are bound, which fused calls go through their
// register and which fused loads nothing reads.
void bindFusedCalls(SymbolTable &symtab);

// Writes the instruction at off in sc holding rel, where bindFusedCalls
// decided that nothing reads a fused load. Returns whether it did, in which
// case the instruction needs no relocation.
bool writeFusedSite(const SectionChunk *sc,
                    const llvm::object::coff_relocation &rel, uint8_t *off);

// Writes, into the contents of sc at buf, each fused call that bindFusedCalls
// decided becomes direct or reads its pointer. A fused call has no
// relocation of its own.
void writeFusedCalls(const SectionChunk *sc, uint8_t *buf);

// Where a fused add that the load holding rel, a REL32 relocation, a local
// import pointer's, serves makes the load take the sum, rewrites the load,
// whose field is at off and which is to become lea, to write the add's
// register, and returns the constant the add adds; zero otherwise.
int64_t takeFusedAdd(const SectionChunk *sc,
                     const llvm::object::coff_relocation &rel, uint8_t *off);

// Adds to edits the changes of size the linker makes to the instructions the
// object of sc, a covered section, describes there, as local imports and
// fused calls are bound.
void addSiteEdits(const SectionChunk *sc, std::vector<RelaxEdit> &edits);

// Keeps each fused access in sc as it is, and each load it reads, where the
// linker does not resize sc after all.
void keepFusedAccesses(const SectionChunk *sc);

// Writes, into the contents of sc at buf, an ARM64 chunk, each fused access
// and add that bindFusedCalls decided reaches the data directly, where the
// offsets the data's address gives them fit their fields.
void writeFusedArm64(const SectionChunk *sc, uint8_t *buf);

// Rewrites the described instruction of form whose REL32 field is at off, at
// RVA p, a reference through the import pointer of a symbol at RVA s, to
// reach the symbol directly. Returns whether the field is left for the
// relocation to fill.
bool rewriteLocalImportSite(uint8_t *off, llvm::COFF::LinkSiteForm form,
                            uint64_t s, uint64_t p);
} // namespace lld::coff

#endif
