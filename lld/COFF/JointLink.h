//===- JointLink.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_JOINTLINK_H
#define LLD_COFF_JOINTLINK_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
struct NewArchiveMember;
namespace opt {
class InputArgList;
} // namespace opt
} // namespace llvm

namespace lld::coff {
class BitcodeFile;
class COFFLinkerContext;
class Symbol;
class SymbolTable;

// Adds to names the shared names of what bitcode file f defines in the COMDAT
// of d and in that of its guard, as a native object's group would hold them.
void addBitcodeGroup(SymbolTable &symtab, BitcodeFile *f, Symbol *d,
                     std::vector<std::string> &names);

// Under /lldjointscan, writes the summary of the member's link, resolved but
// for LTO, and adds the observable vague entities it keeps to the exports of
// the import library the scan writes in place of none.
void writeJointSummary(COFFLinkerContext &ctx,
                       const llvm::opt::InputArgList &args);
void addJointScanOffers(COFFLinkerContext &ctx);

// The scan writes the import library at path where none is there, or where a
// scan wrote it, which the member that marks it says, but never over a final
// link's: the scans of the members that read it then resolve as their final
// links will.
bool mayWriteJointScanImportLibrary(llvm::StringRef path);
void addJointScanMark(COFFLinkerContext &ctx, llvm::StringRef dllName,
                      std::vector<llvm::NewArchiveMember> &members,
                      std::vector<uint8_t> &buffer);

// Under /lldjoint, checks the member's link, resolved but for LTO, against the
// summary its fact file was merged from, and keeps the entities the member
// keeps for the other members.
void applyJointFacts(COFFLinkerContext &ctx,
                     const llvm::opt::InputArgList &args);
void collectJointGrants(COFFLinkerContext &ctx);

// Under -import-slots in an executable, warns of each function it defines
// that a DLL it links, outside its joint link, exports too: on ELF the
// executable's definition would take the DLL's place in the DLL's references.
void warnInterposingDefinitions(COFFLinkerContext &ctx);

// Runs /lldjointmerge.
void mergeJointLink(COFFLinkerContext &ctx,
                    const llvm::opt::InputArgList &args);

// The identity export the merge gives a member that others import from; and a
// DLL member's identity export, which a member that derives a pointer from
// another of the DLL's imports from it, named after the DLL's layout once that
// is final, and the record of that layout in its import library, which such a
// member reads. Returns whether the DLL has the latter, and so writes its
// import library after the image.
bool addJointIdentityExport(COFFLinkerContext &ctx);
void finalizeJointIdentity(COFFLinkerContext &ctx);
void addJointLayoutRecord(COFFLinkerContext &ctx, llvm::StringRef dllName,
                          std::vector<llvm::NewArchiveMember> &members,
                          std::vector<uint8_t> &buffer);

// The loads of the import pointers of a member DLL's data that may be lea of
// the distance from a register holding the pointer of another of its data:
// chosen, with the identity imports they need, before liveness is decided;
// the imports they leave unread left out after; and rewritten with the code.
bool bindJointAnchors(COFFLinkerContext &ctx);
void leaveOutAnchoredImports(COFFLinkerContext &ctx);
void addJointAnchorEdits(COFFLinkerContext &ctx);
} // namespace lld::coff

#endif
