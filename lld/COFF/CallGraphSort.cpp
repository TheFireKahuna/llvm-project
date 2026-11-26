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
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/CallGraphSort.h"
#include <numeric>

using namespace llvm;
using namespace lld;
using namespace lld::coff;

using SectionPair = std::pair<const SectionChunk *, const SectionChunk *>;

// Orders code by its calls: the profile data of /call-graph-ordering-file and
// of the objects, and the jumps that the objects LLVM made for Windows Itanium
// or NT-POSIX state.
//
// This first builds a call graph, whose nodes are sections. An edge is a pair
// that the profile counts: each of its calls that an object states, at its
// offset in its caller, weighs the count divided among them, and a pair with
// no such call is one edge at no offset. Then a jump that ends its section at
// another function's entry joins the two into a chain, heaviest first, a
// section taking one successor and one predecessor: in a row, the jump is
// short or deleted. Of jumps that weigh the same, as all do without a profile,
// one that would be deleted is taken before one that would only be short,
// which could otherwise take its target from it; then the first in the
// objects' order. The chains that the profile's pairs reach are sorted as
// nodes by the chosen algorithm. chainNext takes each chained section's
// successor: a chain that is not sorted stays where its first section is.
DenseMap<const SectionChunk *, int> coff::computeCallGraphProfileOrder(
    COFFLinkerContext &ctx, DenseMap<const Chunk *, const Chunk *> &chainNext) {
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

  // The calls of each caller and callee that the profile counts, and its
  // count for them.
  DenseMap<std::pair<const Chunk *, const Chunk *>,
           std::pair<uint64_t, uint64_t>>
      pairs;

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
    // moving them closer to what calls them. The same holds for the $ groups
    // of an output section, which MinGW does not keep.
    if (ctx.getOutputSection(fromSec) != ctx.getOutputSection(toSec) ||
        (!ctx.config.mingw &&
         fromSec->getSectionName() != toSec->getSectionName()))
      continue;

    pairs[{fromSec, toSec}].second += c.second;
    edges.push_back(
        {getOrCreateNode(fromSec), getOrCreateNode(toSec), c.second});
  }

  // The edges of the jumps that may join chains, and whether each would be
  // deleted.
  std::vector<std::pair<size_t, bool>> jumps;
  // With no pair to count only the jumps that chain take part.
  forEachBranch(ctx, pairs.empty(),
                [&](const SectionChunk &fromSec, uint32_t offset,
                    const SectionChunk &toSec, bool chains, bool deletes) {
                  auto it = pairs.find({&fromSec, &toSec});
                  if (it != pairs.end())
                    ++it->second.first;
                  else if (!chains)
                    return;
                  if (chains)
                    jumps.push_back({edges.size(), deletes});
                  edges.push_back({getOrCreateNode(&fromSec),
                                   getOrCreateNode(&toSec), 0, offset});
                });
  // A pair's calls share its count, and take it from the pair's own edge.
  for (CallGraphEdge &e : edges) {
    auto [calls, count] = pairs.lookup({sections[e.from], sections[e.to]});
    if (calls)
      e.weight =
          e.offset == UINT64_MAX ? 0 : std::max<uint64_t>(count / calls, 1);
  }

  // Join the chains. A node is its chain's first section from here on.
  constexpr uint64_t none = UINT64_MAX;
  std::vector<uint64_t> next(sections.size(), none), chain(sections.size());
  std::vector<bool> follows(sections.size());
  std::iota(chain.begin(), chain.end(), 0);
  auto getChain = [&](uint64_t v) {
    while (chain[v] != v)
      v = chain[v] = chain[chain[v]];
    return v;
  };
  llvm::stable_sort(jumps, [&](auto a, auto b) {
    return std::pair(edges[a.first].weight, a.second) >
           std::pair(edges[b.first].weight, b.second);
  });
  for (auto [i, deletes] : jumps) {
    uint64_t from = edges[i].from, to = edges[i].to;
    if (next[from] != none || follows[to] || getChain(from) == getChain(to))
      continue;
    next[from] = to;
    follows[to] = true;
    chain[to] = getChain(from);
  }
  // A section's offset in its chain, and each chain's size.
  std::vector<uint64_t> offsets(sections.size());
  for (uint64_t i = 0, e = sections.size(); i != e; ++i) {
    if (follows[i])
      continue;
    uint64_t size = 0;
    for (uint64_t j = i; j != none; j = next[j]) {
      offsets[j] = size;
      size += sizes[j];
      chain[j] = i;
    }
    for (uint64_t j = next[i]; j != none; j = next[j])
      sizes[j] = 0;
    sizes[i] = size;
  }
  // The sort is given the weighted edges alone, and places only the chains
  // that they reach.
  llvm::erase_if(edges, [](const CallGraphEdge &e) { return !e.weight; });
  std::vector<bool> placed(sections.size());
  for (CallGraphEdge &e : edges) {
    if (e.offset != UINT64_MAX)
      e.offset += offsets[e.from];
    e.from = chain[e.from];
    e.to = chain[e.to];
    placed[e.from] = placed[e.to] = true;
  }

  // Maximum cluster size in bytes.
  constexpr uint64_t maxClusterSize = 1024 * 1024;
  std::vector<uint64_t> order;
  if (!edges.empty())
    for (uint64_t i : sortByCallGraph(ctx.config.callGraphProfileSort, sizes,
                                      edges, maxClusterSize))
      if (placed[i])
        for (uint64_t j = i; j != none; j = next[j])
          order.push_back(j);
  for (uint64_t i = 0, e = sections.size(); i != e; ++i)
    if (next[i] != none)
      chainNext[sections[i]] = sections[next[i]];

  DenseMap<const SectionChunk *, int> orderMap;
  // Sections will be sorted by increasing order. Absent sections will have
  // priority 0 and be placed at the end of sections. Outside a link that
  // orders code by its branches, only a section that a symbol leads moves, as
  // with /order.
  int curOrder = INT_MIN;
  for (uint64_t i : order)
    if (ctx.hasForkObject || sections[i]->sym)
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
