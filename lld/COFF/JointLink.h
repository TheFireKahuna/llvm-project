//===- JointLink.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_JOINTLINK_H
#define LLD_COFF_JOINTLINK_H

namespace llvm::opt {
class InputArgList;
} // namespace llvm::opt

namespace lld::coff {
class COFFLinkerContext;

// Under /lldjointscan, writes the summary of the member's link, resolved but
// for LTO, and adds the observable vague entities it keeps to the exports of
// the import library the scan writes in place of none.
void writeJointSummary(COFFLinkerContext &ctx,
                       const llvm::opt::InputArgList &args);
void addJointScanOffers(COFFLinkerContext &ctx);

// Under /lldjoint, checks the member's link, resolved but for LTO, against the
// summary its fact file was merged from, and keeps the entities the member
// keeps for the other members.
void applyJointFacts(COFFLinkerContext &ctx,
                     const llvm::opt::InputArgList &args);

// Runs /lldjointmerge.
void mergeJointLink(COFFLinkerContext &ctx,
                    const llvm::opt::InputArgList &args);
} // namespace lld::coff

#endif
