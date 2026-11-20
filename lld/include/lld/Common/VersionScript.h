//===- VersionScript.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_VERSION_SCRIPT_H
#define LLD_COMMON_VERSION_SCRIPT_H

#include "lld/Common/ScriptLexer.h"
#include "lld/Common/Strings.h"
#include <optional>

namespace lld {
struct SymbolVersion {
  llvm::StringRef name;
  bool isExternCpp;
  bool hasWildcard;
};

// This struct contains symbols version definition that
// can be found in version script if it is used for link.
struct VersionDefinition {
  llvm::StringRef name;
  uint16_t id;
  SmallVector<SymbolVersion, 0> nonLocalPatterns;
  SmallVector<SymbolVersion, 0> localPatterns;
};

bool hasWildcard(StringRef s);

// Demangles a symbol name for extern "C++" patterns, keeping its version.
std::string demangleForVersion(StringRef name);

// Reads version nodes into a list of version definitions whose first two
// elements represent VER_NDX_LOCAL and VER_NDX_GLOBAL.
class VersionScriptParser : public ScriptLexer {
public:
  VersionScriptParser(
      CommonLinkerContext &ctx, MemoryBufferRef mb, bool isUnderSysroot,
      llvm::SmallVectorImpl<VersionDefinition> &versionDefinitions)
      : ScriptLexer(ctx, mb, isUnderSysroot),
        versionDefinitions(versionDefinitions) {}

  // Reads a file that holds nothing but version nodes.
  void readVersionScript();
  void readVersionScriptCommand();
  std::pair<SmallVector<SymbolVersion, 0>, SmallVector<SymbolVersion, 0>>
  readSymbols();

private:
  SmallVector<SymbolVersion, 0> readVersionExtern();
  void readAnonymousDeclaration();
  void readVersionDeclaration(StringRef verStr);

  llvm::SmallVectorImpl<VersionDefinition> &versionDefinitions;
};

// Wildcard patterns in precedence order.
class VersionPatternMatcher {
public:
  VersionPatternMatcher() = default;
  // Collects the wildcard patterns of version definitions.
  VersionPatternMatcher(CommonLinkerContext &ctx,
                        ArrayRef<VersionDefinition> versionDefinitions);

  void add(const SymbolVersion &ver, uint16_t versionId) {
    pats.push_back({ver.name, ver.isExternCpp, versionId});
  }
  bool empty() const { return pats.empty(); }
  // Returns the version of the first pattern that matches name.
  std::optional<uint16_t> match(StringRef name) const;

private:
  struct Pattern {
    SingleStringMatcher matcher;
    bool isExternCpp;
    uint16_t versionId;
  };
  SmallVector<Pattern, 0> pats;
};
} // namespace lld

#endif
