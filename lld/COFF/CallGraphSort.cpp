//===- CallGraphSort.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// This builds COFF's call graph for lld/Common/CallGraphSort.cpp, which has
/// the details about the algorithm.
///
//===----------------------------------------------------------------------===//

#include "CallGraphSort.h"
#include "COFFLinkerContext.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/CallGraphSort.h"

using namespace llvm;
using namespace lld;
using namespace lld::coff;

using SectionPair = std::pair<const SectionChunk *, const SectionChunk *>;

// Sort sections by the profile data provided by  /call-graph-ordering-file
//
// This first builds a call graph based on the profile data then merges sections
// according to the C³ heuristic. All clusters are then sorted by a density
// metric to further improve locality.
DenseMap<const SectionChunk *, int>
coff::computeCallGraphProfileOrder(COFFLinkerContext &ctx) {
  std::vector<const SectionChunk *> sections;
  std::vector<uint64_t> sizes;
  std::vector<CallGraphEdge> edges;
  DenseMap<const SectionChunk *, uint64_t> secToNode;

  auto getOrCreateNode = [&](const SectionChunk *isec) -> uint64_t {
    auto res = secToNode.try_emplace(isec, sections.size());
    if (res.second) {
      sections.push_back(isec);
      sizes.push_back(isec->getSize());
    }
    return res.first->second;
  };

  // Take the edge list in Config->CallGraphProfile and generate a graph
  // between InputSections with the provided weights.
  for (const std::pair<SectionPair, uint64_t> &c :
       ctx.config.callGraphProfile) {
    const auto *fromSec = cast<SectionChunk>(c.first.first->repl);
    const auto *toSec = cast<SectionChunk>(c.first.second->repl);

    // Ignore edges between input sections belonging to different output
    // sections.  This is done because otherwise we would end up with clusters
    // containing input sections that can't actually be placed adjacently in the
    // output.  This messes with the cluster size and density calculations.  We
    // would also end up moving input sections in other output sections without
    // moving them closer to what calls them.
    if (ctx.getOutputSection(fromSec) != ctx.getOutputSection(toSec))
      continue;

    uint64_t from = getOrCreateNode(fromSec);
    uint64_t to = getOrCreateNode(toSec);
    edges.push_back({from, to, c.second});
  }

  // Maximum cluster size in bytes.
  constexpr uint64_t maxClusterSize = 1024 * 1024;
  std::vector<uint64_t> order =
      sortByCallGraph(CGProfileSortKind::Hfsort, sizes, edges, maxClusterSize);

  DenseMap<const SectionChunk *, int> orderMap;
  // Sections will be sorted by increasing order. Absent sections will have
  // priority 0 and be placed at the end of sections.
  int curOrder = INT_MIN;
  for (uint64_t i : order)
    orderMap[sections[i]] = curOrder++;

  if (!ctx.config.printSymbolOrder.empty()) {
    std::error_code ec;
    raw_fd_ostream os(ctx.config.printSymbolOrder, ec, sys::fs::OF_None);
    if (ec) {
      Err(ctx) << "cannot open " << ctx.config.printSymbolOrder << ": "
               << ec.message();
      return orderMap;
    }
    // Print the symbols ordered by C3, in the order of increasing curOrder.
    // Search all the symbols in the file of the section
    // and find out a DefinedCOFF symbol with name that is within the
    // section.
    for (uint64_t i : order)
      for (Symbol *sym : sections[i]->file->getSymbols())
        if (auto *d = dyn_cast_or_null<DefinedCOFF>(sym))
          // Filter out non-COMDAT symbols and section symbols.
          if (d->isCOMDAT && !d->getCOFFSymbol().isSection() &&
              sections[i] == d->getChunk())
            os << sym->getName() << "\n";
  }

  return orderMap;
}
