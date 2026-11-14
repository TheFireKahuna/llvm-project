//===- CallGraphSort.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_CALLGRAPHSORT_H
#define LLD_COMMON_CALLGRAPHSORT_H

#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace lld {
// For --call-graph-profile-sort={none,hfsort,cdsort}.
enum class CGProfileSortKind { None, Hfsort, Cdsort };

// A call from node `from` to node `to`, taken `weight` times.
struct CallGraphEdge {
  uint64_t from;
  uint64_t to;
  uint64_t weight;
};

// Returns the nodes of a call graph in layout order. A node is an index into
// `sizes`. `maxClusterSize` bounds a cluster of Call-Chain Clustering, which
// leaves a node of size zero that it merged with no other out of the order.
std::vector<uint64_t> sortByCallGraph(CGProfileSortKind kind,
                                      llvm::ArrayRef<uint64_t> sizes,
                                      llvm::ArrayRef<CallGraphEdge> edges,
                                      uint64_t maxClusterSize);
} // namespace lld

#endif
