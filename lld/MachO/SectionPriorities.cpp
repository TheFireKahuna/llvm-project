//===- SectionPriorities.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// The call graph profile sort is in lld/Common/CallGraphSort.cpp, which has
/// the details about the algorithm.
///
//===----------------------------------------------------------------------===//

#include "SectionPriorities.h"
#include "BPSectionOrderer.h"
#include "Config.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Target.h"

#include "lld/Common/Args.h"
#include "lld/Common/CallGraphSort.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Utils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

using namespace llvm;
using namespace llvm::MachO;
using namespace llvm::sys;
using namespace lld;
using namespace lld::macho;

PriorityBuilder macho::priorityBuilder;

// Sort sections by the profile data provided by __LLVM,__cg_profile sections.
//
// This first builds a call graph based on the profile data then merges
// sections according to the C³ heuristic. All clusters are then sorted by a
// density metric to further improve locality.
static DenseMap<const InputSection *, int>
computeCallGraphProfileOrder(const MapVector<SectionPair, uint64_t> &profile) {
  std::vector<const InputSection *> sections;
  std::vector<uint64_t> sizes;
  std::vector<CallGraphEdge> edges;
  DenseMap<const InputSection *, uint64_t> secToNode;

  auto getOrCreateNode = [&](const InputSection *isec) -> uint64_t {
    auto res = secToNode.try_emplace(isec, sections.size());
    if (res.second) {
      sections.push_back(isec);
      sizes.push_back(isec->getSize());
    }
    return res.first->second;
  };

  // Take the edge list in callGraphProfile and generate a graph between
  // InputSections with the provided weights.
  for (const std::pair<SectionPair, uint64_t> &c : profile) {
    const auto fromSec = c.first.first->canonical();
    const auto toSec = c.first.second->canonical();
    // Ignore edges between input sections belonging to different output
    // sections.  This is done because otherwise we would end up with clusters
    // containing input sections that can't actually be placed adjacently in the
    // output.  This messes with the cluster size and density calculations.  We
    // would also end up moving input sections in other output sections without
    // moving them closer to what calls them.
    if (fromSec->parent != toSec->parent)
      continue;

    uint64_t from = getOrCreateNode(fromSec);
    uint64_t to = getOrCreateNode(toSec);
    edges.push_back({from, to, c.second});
  }

  std::vector<uint64_t> order = sortByCallGraph(
      CGProfileSortKind::Hfsort, sizes, edges, target->getPageSize());

  DenseMap<const InputSection *, int> orderMap;
  // Sections will be sorted by decreasing order. Absent sections will have
  // priority 0 and be placed at the end of sections.
  int curOrder = -sections.size();
  for (uint64_t i : order)
    orderMap[sections[i]] = curOrder++;

  if (!config->printSymbolOrder.empty()) {
    std::error_code ec;
    raw_fd_ostream os(config->printSymbolOrder, ec, sys::fs::OF_None);
    if (ec) {
      error("cannot open " + config->printSymbolOrder + ": " + ec.message());
      return orderMap;
    }
    // Print the symbols ordered by C3, in the order of decreasing curOrder.
    // Search all the symbols in the file of the section
    // and find out a Defined symbol with name that is within the
    // section.
    for (uint64_t i : order)
      for (Symbol *sym : sections[i]->getFile()->symbols)
        if (auto *d = dyn_cast_or_null<Defined>(sym))
          if (d->isec() == sections[i])
            os << sym->getName() << "\n";
  }

  return orderMap;
}

void macho::PriorityBuilder::SymbolPriorityEntry::setPriority(
    int priority, StringRef objectFile) {
  if (!objectFile.empty())
    objectFiles.try_emplace(objectFile, priority);
  else
    anyObjectFile = std::min(anyObjectFile, priority);
}

int macho::PriorityBuilder::SymbolPriorityEntry::getPriority(
    const InputFile *f) const {
  if (!f)
    return anyObjectFile;
  // We don't use toString(InputFile *) here because it returns the full path
  // for object files, and we only want the basename.
  StringRef basename = path::filename(f->getName());
  StringRef filename =
      f->archiveName.empty()
          ? basename
          : saver().save(path::filename(f->archiveName) + "(" + basename + ")");
  return std::min(objectFiles.lookup(filename), anyObjectFile);
}

std::optional<int>
macho::PriorityBuilder::getCStringPriority(uint32_t hash,
                                           const InputFile *f) const {
  auto it = cStringPriorities.find(hash);
  if (it == cStringPriorities.end())
    return std::nullopt;
  return it->second.getPriority(f);
}

std::optional<int>
macho::PriorityBuilder::getSymbolPriority(const Defined *sym) const {
  if (sym->isAbsolute())
    return std::nullopt;
  auto it = priorities.find(utils::getRootSymbol(sym->getName()));
  if (it == priorities.end())
    return std::nullopt;
  return it->second.getPriority(sym->isec()->getFile());
}

void macho::PriorityBuilder::extractCallGraphProfile() {
  TimeTraceScope timeScope("Extract call graph profile");
  bool hasOrderFile = !priorities.empty();
  for (const InputFile *file : inputFiles) {
    auto *obj = dyn_cast_or_null<ObjFile>(file);
    if (!obj)
      continue;
    for (const CallGraphEntry &entry : obj->callGraph) {
      assert(entry.fromIndex < obj->symbols.size() &&
             entry.toIndex < obj->symbols.size());
      auto *fromSym = dyn_cast_or_null<Defined>(obj->symbols[entry.fromIndex]);
      auto *toSym = dyn_cast_or_null<Defined>(obj->symbols[entry.toIndex]);
      if (fromSym && toSym &&
          (!hasOrderFile ||
           (!getSymbolPriority(fromSym) && !getSymbolPriority(toSym))))
        callGraphProfile[{fromSym->isec(), toSym->isec()}] += entry.count;
    }
  }
}

void macho::PriorityBuilder::parseOrderFile(StringRef path) {
  assert(callGraphProfile.empty() &&
         "Order file must be parsed before call graph profile is processed");
  std::optional<MemoryBufferRef> buffer = readFile(path);
  if (!buffer) {
    error("Could not read order file at " + path);
    return;
  }

  int prio = std::numeric_limits<int>::min();
  MemoryBufferRef mbref = *buffer;
  for (StringRef line : args::getLines(mbref)) {
    StringRef objectFile;
    line = line.take_until([](char c) { return c == '#'; }); // ignore comments
    line = line.ltrim();

    CPUType cpuType = StringSwitch<CPUType>(line)
                          .StartsWith("i386:", CPU_TYPE_I386)
                          .StartsWith("x86_64:", CPU_TYPE_X86_64)
                          .StartsWith("arm:", CPU_TYPE_ARM)
                          .StartsWith("arm64:", CPU_TYPE_ARM64)
                          .StartsWith("ppc:", CPU_TYPE_POWERPC)
                          .StartsWith("ppc64:", CPU_TYPE_POWERPC64)
                          .Default(CPU_TYPE_ANY);

    if (cpuType != CPU_TYPE_ANY && cpuType != target->cpuType)
      continue;
    // Drop the CPU type as well as the colon
    if (cpuType != CPU_TYPE_ANY)
      line = line.drop_until([](char c) { return c == ':'; }).drop_front();

    constexpr std::array<StringRef, 2> fileEnds = {".o:", ".o):"};
    for (StringRef fileEnd : fileEnds) {
      size_t pos = line.find(fileEnd);
      if (pos != StringRef::npos) {
        // Split the string around the colon
        objectFile = line.take_front(pos + fileEnd.size() - 1);
        line = line.drop_front(pos + fileEnd.size());
        break;
      }
    }

    // The rest of the line is either <symbol name> or
    // cStringEntryPrefix<cstring hash>
    line = line.trim();
    if (line.consume_front(cStringEntryPrefix)) {
      uint32_t hash = 0;
      // Only accept hex (0x prefix) or decimal format
      if (line.consume_front_insensitive("0x") ? !line.getAsInteger(16, hash)
                                               : !line.getAsInteger(10, hash))
        cStringPriorities[hash].setPriority(prio, objectFile);
    } else {
      StringRef symbol = utils::getRootSymbol(line);
      if (!symbol.empty())
        priorities[symbol].setPriority(prio, objectFile);
    }

    ++prio;
  }
}

DenseMap<const InputSection *, int>
macho::PriorityBuilder::buildInputSectionPriorities() {
  DenseMap<const InputSection *, int> sectionPriorities;
  if (config->bpStartupFunctionSort || config->bpFunctionOrderForCompression ||
      config->bpDataOrderForCompression ||
      !config->bpCompressionSortSpecs.empty()) {
    TimeTraceScope timeScope("Balanced Partitioning Section Orderer");
    sectionPriorities = runBalancedPartitioning(
        config->bpStartupFunctionSort ? config->irpgoProfilePath : "",
        config->bpCompressionSortSpecs, config->bpFunctionOrderForCompression,
        config->bpDataOrderForCompression,
        config->bpCompressionSortStartupFunctions,
        config->bpVerboseSectionOrderer);
  } else if (config->callGraphProfileSort) {
    TimeTraceScope timeScope("Call graph profile sort");
    sectionPriorities = computeCallGraphProfileOrder(callGraphProfile);
  }

  if (priorities.empty())
    return sectionPriorities;

  auto addSym = [&](const Defined *sym) {
    std::optional<int> symbolPriority = getSymbolPriority(sym);
    if (!symbolPriority)
      return;
    auto *isec = sym->isec();
    int &priority = sectionPriorities[isec];
    priority = std::min(priority, *symbolPriority);
    // Order file takes precedence over cold partitioning.
    isec->isCold = false;
  };

  // TODO: Make sure this handles weak symbols correctly.
  for (const InputFile *file : inputFiles) {
    if (isa<ObjFile>(file))
      for (Symbol *sym : file->symbols)
        if (auto *d = dyn_cast_or_null<Defined>(sym))
          addSym(d);
  }

  return sectionPriorities;
}

void macho::PriorityBuilder::forEachStringPiece(
    ArrayRef<CStringInputSection *> inputs,
    std::function<void(CStringInputSection &, StringPiece &, size_t)> f,
    bool forceInputOrder, bool computeHash) const {
  std::vector<std::tuple<int, CStringInputSection *, size_t>> orderedPieces;
  std::vector<std::pair<CStringInputSection *, size_t>> unorderedPieces;
  for (CStringInputSection *isec : inputs) {
    for (const auto &[stringPieceIdx, piece] : llvm::enumerate(isec->pieces)) {
      if (!piece.live)
        continue;
      // Process pieces in input order if we have no cstrings in our orderfile
      if (forceInputOrder || cStringPriorities.empty()) {
        f(*isec, piece, stringPieceIdx);
        continue;
      }
      uint32_t hash =
          computeHash
              ? (xxh3_64bits(isec->getStringRef(stringPieceIdx)) & 0x7fffffff)
              : piece.hash;
      if (auto priority = getCStringPriority(hash, isec->getFile()))
        orderedPieces.emplace_back(*priority, isec, stringPieceIdx);
      else
        unorderedPieces.emplace_back(isec, stringPieceIdx);
    }
  }
  if (orderedPieces.empty() && unorderedPieces.empty())
    return;
  llvm::stable_sort(orderedPieces, [](const auto &left, const auto &right) {
    return std::get<0>(left) < std::get<0>(right);
  });
  for (auto &[priority, isec, pieceIdx] : orderedPieces)
    f(*isec, isec->pieces[pieceIdx], pieceIdx);
  // TODO: Add option to order the remaining cstrings for compression
  for (auto &[isec, pieceIdx] : unorderedPieces)
    f(*isec, isec->pieces[pieceIdx], pieceIdx);
}
