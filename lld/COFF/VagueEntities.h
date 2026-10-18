//===- VagueEntities.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_VAGUE_ENTITIES_H
#define LLD_COFF_VAGUE_ENTITIES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lld::coff {
class COFFLinkerContext;
class DefinedRegular;

// Prefix of the symbol naming an import library's record of a DLL.
inline constexpr llvm::StringLiteral vagueRecordPrefix = "__llvm_vague$";

// Under -import-slots, reports each pair of images in imageName's load closure
// that both keep an observable default-visibility vague entity, which ELF
// unifies: siblings no import path joins, or an importer keeping its own copy.
void checkVagueEntities(COFFLinkerContext &ctx, llvm::StringRef imageName);

// Under -import-slots, the vague entities two images of an executable's load
// closure keep preemptible (the executable's own are in kept), each with its
// owner's DLL name: empty for the executable, else the first keeper in
// breadth-first load order, as ELF's global scope gives.
std::vector<std::pair<llvm::StringRef, llvm::StringRef>>
chooseVagueOwners(COFFLinkerContext &ctx,
                  const llvm::DenseSet<llvm::StringRef> &kept);

// Under -import-slots, the preemptible vague entities of each DLL the image
// imports from, by lower-case DLL name; before liveness, every DLL read from.
llvm::StringMap<llvm::StringSet<>> getPreemptibleOffers(COFFLinkerContext &ctx,
                                                        bool liveOnly = true);

// The overridable defaults offered by the DLLs of the load closure described by
// records (import-library members in link order), each with its DLL's name, in
// breadth-first load order, excluding the DLLs whose libraries hold records.
std::vector<std::pair<llvm::StringRef, llvm::StringRef>>
getDependentOverrideOffers(COFFLinkerContext &ctx,
                           llvm::ArrayRef<llvm::MemoryBufferRef> records);

// Under -import-slots, adds to dllName's import library a member recording the
// entities it keeps and the DLLs it imports from with their records; buffer
// holds the member.
void addVagueEntityRecord(COFFLinkerContext &ctx, llvm::StringRef dllName,
                          std::vector<llvm::NewArchiveMember> &members,
                          std::vector<uint8_t> &buffer);

// Under -import-slots and -offers-baseline, reports each entity dllName now
// keeps that the previous release (named by the option) did not: a DLL linked
// against that release keeps its own copy.
void checkOfferAdditions(COFFLinkerContext &ctx, llvm::StringRef dllName);

// The DLL's epoch export for -offers-epoch, changed when its entities change.
llvm::StringRef getOffersEpoch(COFFLinkerContext &ctx);

// Under -import-slots and -offers-epoch, a DLL that offers data exports the
// epoch of the entities it keeps.
void addOffersEpoch(COFFLinkerContext &ctx);

// Under -import-slots, a DLL that offers a shared entity imports each imported
// DLL's epoch, so a release of that DLL with more offers refuses its load.
void importOffersEpochs(COFFLinkerContext &ctx);

// Whether a COMDAT data definition has default visibility, so a DLL's instance
// may preempt it, as on ELF.
bool isDefaultVisibility(DefinedRegular *d);
} // namespace lld::coff

#endif
