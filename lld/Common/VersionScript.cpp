//===- VersionScript.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/VersionScript.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace lld;

void VersionScriptParser::readVersionScriptCommand() {
  SaveAndRestore saved(lexState, State::VersionNode);
  if (consume("{")) {
    readAnonymousDeclaration();
    return;
  }

  if (atEOF())
    setError("unexpected EOF");
  while (peek() != "}" && !atEOF()) {
    StringRef verStr = next();
    if (verStr == "{") {
      setError("anonymous version definition is used in "
               "combination with other version definitions");
      return;
    }
    expect("{");
    readVersionDeclaration(verStr);
  }
}

// Reads an anonymous version declaration.
void VersionScriptParser::readAnonymousDeclaration() {
  SmallVector<SymbolVersion, 0> locals;
  SmallVector<SymbolVersion, 0> globals;
  std::tie(locals, globals) = readSymbols();
  for (const SymbolVersion &pat : locals)
    versionDefinitions[VER_NDX_LOCAL].localPatterns.push_back(pat);
  for (const SymbolVersion &pat : globals)
    versionDefinitions[VER_NDX_GLOBAL].nonLocalPatterns.push_back(pat);

  expect(";");
}

// Reads a non-anonymous version definition,
// e.g. "VerStr { global: foo; bar; local: *; };".
void VersionScriptParser::readVersionDeclaration(StringRef verStr) {
  // Read a symbol list.
  SmallVector<SymbolVersion, 0> locals;
  SmallVector<SymbolVersion, 0> globals;
  std::tie(locals, globals) = readSymbols();

  // Create a new version definition and add that to the global symbols.
  VersionDefinition ver;
  ver.name = verStr;
  ver.nonLocalPatterns = std::move(globals);
  ver.localPatterns = std::move(locals);
  ver.id = versionDefinitions.size();
  versionDefinitions.push_back(ver);

  // Each version may have a parent version. For example, "Ver2"
  // defined as "Ver2 { global: foo; local: *; } Ver1;" has "Ver1"
  // as a parent. This version hierarchy is, probably against your
  // instinct, purely for hint; the runtime doesn't care about it
  // at all. In LLD, we simply ignore it.
  if (next() != ";")
    expect(";");
}

bool lld::hasWildcard(StringRef s) {
  return s.find_first_of("?*[") != StringRef::npos;
}

// Reads a list of symbols, e.g. "{ global: foo; bar; local: *; };".
std::pair<SmallVector<SymbolVersion, 0>, SmallVector<SymbolVersion, 0>>
VersionScriptParser::readSymbols() {
  SmallVector<SymbolVersion, 0> locals;
  SmallVector<SymbolVersion, 0> globals;
  SmallVector<SymbolVersion, 0> *v = &globals;

  while (auto tok = till("}")) {
    if (tok == "extern") {
      SmallVector<SymbolVersion, 0> ext = readVersionExtern();
      v->insert(v->end(), ext.begin(), ext.end());
    } else {
      if (tok == "local" && consume(":")) {
        v = &locals;
        continue;
      }
      if (tok == "global" && consume(":")) {
        v = &globals;
        continue;
      }
      v->push_back({unquote(tok), false, hasWildcard(tok)});
    }
    expect(";");
  }
  return {locals, globals};
}

// Reads an "extern C++" directive, e.g.,
// "extern "C++" { ns::*; "f(int, double)"; };"
//
// The last semicolon is optional. E.g. this is OK:
// "extern "C++" { ns::*; "f(int, double)" };"
SmallVector<SymbolVersion, 0> VersionScriptParser::readVersionExtern() {
  StringRef tok = next();
  bool isCXX = tok == "\"C++\"";
  if (!isCXX && tok != "\"C\"")
    setError("Unknown language");
  expect("{");

  SmallVector<SymbolVersion, 0> ret;
  while (auto tok = till("}")) {
    ret.push_back(
        {unquote(tok), isCXX, !tok.str.starts_with("\"") && hasWildcard(tok)});
    if (consume("}"))
      return ret;
    expect(";");
  }
  return ret;
}

std::string lld::demangleForVersion(StringRef name) {
  auto [base, ver] = name.split('@');
  std::string s = demangle(base);
  if (!ver.empty() && !ver.starts_with('@'))
    s += ("@" + ver).str();
  return s;
}

// Collect wildcards in precedence order, where "*" patterns have the lowest
// precedence in GNU ld. Because the last match takes precedence over previous
// matches, we iterate over the definitions in the reverse order.
VersionPatternMatcher::VersionPatternMatcher(
    CommonLinkerContext &ctx, ArrayRef<VersionDefinition> versionDefinitions) {
  SmallVector<Pattern, 0> asterisks;
  bool globalAsteriskFound = false;
  bool localAsteriskFound = false;
  bool asteriskReported = false;
  for (const VersionDefinition &v : llvm::reverse(versionDefinitions)) {
    for (bool isLocal : {false, true}) {
      uint16_t id = isLocal ? VER_NDX_LOCAL : v.id;
      for (const SymbolVersion &pat :
           isLocal ? v.localPatterns : v.nonLocalPatterns) {
        if (!pat.hasWildcard)
          continue;
        if (pat.name != "*") {
          add(pat, id);
          continue;
        }
        if (!asteriskReported) {
          if ((isLocal && globalAsteriskFound) ||
              (!isLocal && localAsteriskFound)) {
            SyncStream(ctx.e, DiagLevel::Warn).os
                << "wildcard pattern '*' is used for both 'local' and 'global' "
                   "scopes in version script";
            asteriskReported = true;
          } else if (!isLocal && globalAsteriskFound) {
            SyncStream(ctx.e, DiagLevel::Warn).os
                << "wildcard pattern '*' is used for multiple version "
                   "definitions in version script";
            asteriskReported = true;
          } else {
            localAsteriskFound = isLocal;
            globalAsteriskFound = !isLocal;
          }
        }
        asterisks.push_back({pat.name, pat.isExternCpp, id});
      }
    }
  }
  pats.append(asterisks);
}

std::optional<uint16_t> VersionPatternMatcher::match(StringRef name) const {
  std::optional<std::string> demangled;
  for (const Pattern &pat : pats) {
    if (pat.isExternCpp && !demangled)
      demangled = demangleForVersion(name);
    if (pat.matcher.match(pat.isExternCpp ? StringRef(*demangled) : name))
      return pat.versionId;
  }
  return std::nullopt;
}
