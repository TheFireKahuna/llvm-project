//===- CallGraphSort.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// This builds ELF's call graph for lld/Common/CallGraphSort.cpp, which has the
/// details about the algorithm.
///
//===----------------------------------------------------------------------===//

#include "CallGraphSort.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Symbols.h"
#include "lld/Common/CallGraphSort.h"
#include "llvm/Support/FileSystem.h"

using namespace llvm;
using namespace lld;
using namespace lld::elf;

using SectionPair =
    std::pair<const InputSectionBase *, const InputSectionBase *>;

// Sort sections by the profile data provided by --callgraph-profile-file.
//
// This first builds a call graph based on the profile data then merges sections
// according either to the C³ or Cache-Directed-Sort ordering algorithm.
DenseMap<const InputSectionBase *, int>
elf::computeCallGraphProfileOrder(Ctx &ctx) {
  bool cdsort = ctx.arg.callGraphProfileSort == CGProfileSortKind::Cdsort;
  SmallVector<const InputSectionBase *, 0> sections;
  SmallVector<uint64_t, 0> sizes;
  SmallVector<CallGraphEdge, 0> edges;
  DenseMap<const InputSectionBase *, uint64_t> secToNode;

  auto getOrCreateNode = [&](const InputSectionBase *isec) -> uint64_t {
    auto res = secToNode.try_emplace(isec, sections.size());
    if (res.second) {
      sections.push_back(isec);
      sizes.push_back(isec->getSize());
    }
    return res.first->second;
  };

  // Take the edge list in ctx.arg.callGraphProfile and generate a graph between
  // InputSections with the provided weights.
  for (std::pair<SectionPair, uint64_t> &c : ctx.arg.callGraphProfile) {
    const auto *fromSB = cast<InputSectionBase>(c.first.first);
    const auto *toSB = cast<InputSectionBase>(c.first.second);

    // Ignore edges between input sections belonging to different output
    // sections.  This is done because otherwise we would end up with clusters
    // containing input sections that can't actually be placed adjacently in the
    // output.  This messes with the cluster size and density calculations.  We
    // would also end up moving input sections in other output sections without
    // moving them closer to what calls them.
    if (fromSB->getOutputSection() != toSB->getOutputSection())
      continue;

    // Cache-Directed Sort ignores edges with zero weight, and so does not
    // place a section that has no other edge.
    if (cdsort && c.second == 0)
      continue;

    uint64_t from = getOrCreateNode(fromSB);
    uint64_t to = getOrCreateNode(toSB);
    edges.push_back({from, to, c.second});
  }

  // Maximum cluster size in bytes.
  constexpr uint64_t maxClusterSize = 1024 * 1024;
  std::vector<uint64_t> order = sortByCallGraph(ctx.arg.callGraphProfileSort,
                                                sizes, edges, maxClusterSize);

  DenseMap<const InputSectionBase *, int> orderMap;
  int curOrder = -sections.size();
  for (uint64_t i : order)
    orderMap[sections[i]] = curOrder++;

  if (!cdsort && !ctx.arg.printSymbolOrder.empty()) {
    std::error_code ec;
    raw_fd_ostream os(ctx.arg.printSymbolOrder, ec, sys::fs::OF_None);
    if (ec) {
      ErrAlways(ctx) << "cannot open " << ctx.arg.printSymbolOrder << ": "
                     << ec.message();
      return orderMap;
    }

    // Print the symbols ordered by C3, in the order of increasing curOrder.
    // Search all the symbols in the file of the section
    // and find out a Defined symbol with name that is within the section.
    for (uint64_t i : order)
      for (Symbol *sym : sections[i]->file->getSymbols())
        if (!sym->isSection()) // Filter out section-type symbols here.
          if (auto *d = dyn_cast<Defined>(sym))
            if (sections[i] == d->section)
              os << sym->getName() << "\n";
  }

  return orderMap;
}
