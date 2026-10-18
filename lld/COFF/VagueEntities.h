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

// The prefix of the name by which an import library's record of the DLL it
// imports from is found, which the DLL's name follows.
inline constexpr llvm::StringLiteral vagueRecordPrefix = "__llvm_vague$";

// Under -import-slots, reports each pair of images of the load closure of the
// image imageName, as the records of the DLLs it imports describe it, that
// both keep an observable vague entity with default visibility: siblings that
// no import path joins, or an image that keeps its copy while importing from
// one that defines the entity.
void checkVagueEntities(COFFLinkerContext &ctx, llvm::StringRef imageName);

// Under -import-slots, the vague entities that an executable's link sees two
// images keep preemptible, among the images of its load closure, whose records
// the link reads, and the executable itself, which keeps those in kept: each
// with its owner's DLL name, or an empty name for the executable, which owns
// what it keeps, and otherwise the first to keep it in breadth-first load
// order, as ELF's global scope gives.
std::vector<std::pair<llvm::StringRef, llvm::StringRef>>
chooseVagueOwners(COFFLinkerContext &ctx,
                  const llvm::DenseSet<llvm::StringRef> &kept);

// Under -import-slots, the vague entities that each DLL the image imports from
// keeps preemptible, as its record describes them, by the DLL's name in lower
// case; before liveness is decided, of every DLL an import was read from.
llvm::StringMap<llvm::StringSet<>> getPreemptibleOffers(COFFLinkerContext &ctx,
                                                        bool liveOnly = true);

// The overridable defaults that the DLLs of the load closure that records, the
// record members of import libraries in link order, describe offer, each with
// the name of its DLL, in breadth-first load order, leaving out those of the
// DLLs whose import libraries hold the records.
std::vector<std::pair<llvm::StringRef, llvm::StringRef>>
getDependentOverrideOffers(COFFLinkerContext &ctx,
                           llvm::ArrayRef<llvm::MemoryBufferRef> records);

// Under -import-slots, adds to the import library of the DLL dllName a member
// that records the entities it keeps, the DLLs it imports from and their
// records. The member refers to buffer, which holds it.
void addVagueEntityRecord(COFFLinkerContext &ctx, llvm::StringRef dllName,
                          std::vector<llvm::NewArchiveMember> &members,
                          std::vector<uint8_t> &buffer);

// Under -import-slots and -offers-baseline, reports each entity the DLL dllName
// keeps that the previous release, whose import library the option names, did
// not: a DLL linked against that import library keeps its own copy.
void checkOfferAdditions(COFFLinkerContext &ctx, llvm::StringRef dllName);

// The name of the epoch export of the DLL, which changes when the entities it
// keeps change, for -offers-epoch.
llvm::StringRef getOffersEpoch(COFFLinkerContext &ctx);

// Under -import-slots and -offers-epoch, in a DLL that offers data, exports
// the epoch of the entities it keeps.
void addOffersEpoch(COFFLinkerContext &ctx);

// Under -import-slots, in a DLL that offers a shared entity, imports the epoch
// of each DLL it imports from that exports one, so that a release of that DLL
// with more offers refuses this DLL's load.
void importOffersEpochs(COFFLinkerContext &ctx);

// Whether a data definition in a COMDAT has default visibility, so that a
// DLL's instance may preempt it, as on ELF.
bool isDefaultVisibility(DefinedRegular *d);
} // namespace lld::coff

#endif
