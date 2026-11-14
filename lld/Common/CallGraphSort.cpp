//===- CallGraphSort.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// The file is responsible for sorting sections using LLVM call graph profile
/// data by placing frequently executed code sections together. The goal of the
/// placement is to improve the runtime performance of the final executable by
/// arranging code sections so that i-TLB misses and i-cache misses are reduced.
///
/// Each linker builds a call graph based on the profile data. The algorithm
/// then iteratively merges "chains" (ordered lists) of input sections which
/// will be laid out as a unit. There are two implementations for deciding how
/// to merge a pair of chains:
///  - a simpler one, referred to as Call-Chain Clustering (C^3), that follows
///    "Optimizing Function Placement for Large-Scale Data-Center Applications"
/// https://research.fb.com/wp-content/uploads/2017/01/cgo2017-hfsort-final1.pdf
/// - a more advanced one, referred to as Cache-Directed-Sort (CDSort), which
///   typically produces layouts with higher locality, and hence, yields fewer
///   instruction cache misses on large binaries.
//===----------------------------------------------------------------------===//

#include "lld/Common/CallGraphSort.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Transforms/Utils/CodeLayout.h"

#include <numeric>

using namespace llvm;
using namespace lld;

namespace {
struct Edge {
  int from;
  uint64_t weight;
};

struct Cluster {
  Cluster(int sec, uint64_t s) : next(sec), prev(sec), size(s) {}

  double getDensity() const {
    if (size == 0)
      return 0;
    return double(weight) / double(size);
  }

  int next;
  int prev;
  uint64_t size;
  uint64_t weight = 0;
  uint64_t initialWeight = 0;
  Edge bestPred = {-1, 0};
};

// Maximum amount the combined cluster density can be worse than the original
// cluster to consider merging.
constexpr int MAX_DENSITY_DEGRADATION = 8;
} // end anonymous namespace

// It's bad to merge clusters which would degrade the density too much.
static bool isNewDensityBad(Cluster &a, Cluster &b) {
  double newDensity = double(a.weight + b.weight) / double(a.size + b.size);
  return newDensity < a.getDensity() / MAX_DENSITY_DEGRADATION;
}

// Find the leader of V's belonged cluster (represented as an equivalence
// class). We apply union-find path-halving technique (simple to implement) in
// the meantime as it decreases depths and the time complexity.
static int getLeader(std::vector<int> &leaders, int v) {
  while (leaders[v] != v) {
    leaders[v] = leaders[leaders[v]];
    v = leaders[v];
  }
  return v;
}

static void mergeClusters(std::vector<Cluster> &cs, Cluster &into, int intoIdx,
                          Cluster &from, int fromIdx) {
  int tail1 = into.prev, tail2 = from.prev;
  into.prev = tail2;
  cs[tail2].next = intoIdx;
  from.prev = tail1;
  cs[tail1].next = fromIdx;
  into.size += from.size;
  into.weight += from.weight;
  from.size = 0;
  from.weight = 0;
}

/// Implementation of the Call-Chain Clustering (C^3). The goal of this
/// algorithm is to improve runtime performance of the executable by arranging
/// code sections such that page table and i-cache misses are minimized.
///
/// Definitions:
/// * Cluster
///   * An ordered list of input sections which are laid out as a unit. At the
///     beginning of the algorithm each input section has its own cluster and
///     the weight of the cluster is the sum of the weight of all incoming
///     edges.
/// * Call-Chain Clustering (C³) Heuristic
///   * Defines when and how clusters are combined. Pick the highest weighted
///     input section then add it to its most likely predecessor if it wouldn't
///     penalize it too much.
/// * Density
///   * The weight of the cluster divided by the size of the cluster. This is a
///     proxy for the amount of execution time spent per byte of the cluster.
///
/// It does so given a weighted call graph by the following:
/// * Sort input sections by weight
/// * For each input section starting with the highest weight
///   * Find its most likely predecessor cluster
///   * Check if the combined cluster would be too large, or would have too low
///     a density.
///   * If not, then combine the clusters.
/// * Sort non-empty clusters by density
static std::vector<uint64_t> callChainClustering(ArrayRef<uint64_t> sizes,
                                                 ArrayRef<CallGraphEdge> edges,
                                                 uint64_t maxClusterSize) {
  std::vector<Cluster> clusters;
  clusters.reserve(sizes.size());
  for (uint64_t size : sizes)
    clusters.emplace_back(clusters.size(), size);

  for (const CallGraphEdge &e : edges) {
    Cluster &toC = clusters[e.to];
    toC.weight += e.weight;

    if (e.from == e.to)
      continue;

    // Remember the best edge.
    if (toC.bestPred.from == -1 || toC.bestPred.weight < e.weight) {
      toC.bestPred.from = e.from;
      toC.bestPred.weight = e.weight;
    }
  }
  for (Cluster &c : clusters)
    c.initialWeight = c.weight;

  // Cluster indices sorted by density.
  std::vector<int> sorted(clusters.size());
  // For union-find.
  std::vector<int> leaders(clusters.size());

  std::iota(leaders.begin(), leaders.end(), 0);
  std::iota(sorted.begin(), sorted.end(), 0);
  llvm::stable_sort(sorted, [&](int a, int b) {
    return clusters[a].getDensity() > clusters[b].getDensity();
  });

  for (int l : sorted) {
    // The cluster index is the same as the index of its leader here because
    // clusters[L] has not been merged into another cluster yet.
    Cluster &c = clusters[l];

    // Don't consider merging if the edge is unlikely.
    if (c.bestPred.from == -1 || c.bestPred.weight * 10 <= c.initialWeight)
      continue;

    int predL = getLeader(leaders, c.bestPred.from);
    // Already in the same cluster.
    if (l == predL)
      continue;

    Cluster *predC = &clusters[predL];
    if (c.size + predC->size > maxClusterSize)
      continue;

    if (isNewDensityBad(*predC, c))
      continue;

    leaders[l] = predL;
    mergeClusters(clusters, *predC, predL, c, l);
  }

  // Sort remaining non-empty clusters by density.
  sorted.clear();
  for (int i = 0, e = (int)clusters.size(); i != e; ++i)
    if (clusters[i].size > 0)
      sorted.push_back(i);
  llvm::stable_sort(sorted, [&](int a, int b) {
    return clusters[a].getDensity() > clusters[b].getDensity();
  });

  std::vector<uint64_t> order;
  for (int leader : sorted) {
    for (int i = leader;;) {
      order.push_back(i);
      i = clusters[i].next;
      if (i == leader)
        break;
    }
  }
  return order;
}

// Sort nodes using the Cache-Directed Sort algorithm. The placement is done by
// optimizing the locality by co-locating frequently executed code sections
// together.
static std::vector<uint64_t> cacheDirectedSort(ArrayRef<uint64_t> sizes,
                                               ArrayRef<CallGraphEdge> edges) {
  SmallVector<uint64_t, 0> funcCounts(sizes.size());
  SmallVector<codelayout::EdgeCount, 0> callCounts;
  SmallVector<uint64_t, 0> callOffsets;

  for (const CallGraphEdge &e : edges) {
    // Ignore self-edges (recursive calls).
    if (e.from == e.to)
      continue;

    callCounts.push_back({e.from, e.to, e.weight});
    // Assume that the jump is at the middle of the input section. The profile
    // data does not contain jump offsets.
    callOffsets.push_back((sizes[e.from] + 1) / 2);
    funcCounts[e.to] += e.weight;
  }

  return codelayout::computeCacheDirectedLayout(sizes, funcCounts, callCounts,
                                                callOffsets);
}

std::vector<uint64_t> lld::sortByCallGraph(CGProfileSortKind kind,
                                           ArrayRef<uint64_t> sizes,
                                           ArrayRef<CallGraphEdge> edges,
                                           uint64_t maxClusterSize) {
  if (kind == CGProfileSortKind::Cdsort)
    return cacheDirectedSort(sizes, edges);
  return callChainClustering(sizes, edges, maxClusterSize);
}
