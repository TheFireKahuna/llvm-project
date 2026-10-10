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

namespace lld::coff {
class Defined;
class DefinedLocalImport;
class ObjFile;
class SectionChunk;
class SymbolTable;

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

// Rewrites the described instruction of form whose REL32 field is at off, at
// RVA p, a reference through the import pointer of a symbol at RVA s, to
// reach the symbol directly. Returns whether the field is left for the
// relocation to fill.
bool rewriteLocalImportSite(uint8_t *off, llvm::COFF::LinkSiteForm form,
                            uint64_t s, uint64_t p);
} // namespace lld::coff

#endif
