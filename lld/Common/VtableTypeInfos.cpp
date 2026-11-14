//===- VtableTypeInfos.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/VtableTypeInfos.h"
#include "lld/Common/CommonLinkerContext.h"
#include "llvm/Support/GlobPattern.h"

using namespace llvm;
using namespace lld;

bool VtableTypeInfoCheck::run(CommonLinkerContext &ctx,
                              ArrayRef<StringRef> knownSafeVtables) {
  SmallSetVector<StringRef, 0> vtableSymbolsWithNoRTTI;
  for (StringRef s : vtableSymbols)
    if (!typeInfoSymbols.contains(s))
      vtableSymbolsWithNoRTTI.insert(s);

  // Remove known safe symbols.
  for (StringRef knownSafeName : knownSafeVtables) {
    if (!knownSafeName.consume_front("_ZTV"))
      SyncStream(ctx.e, DiagLevel::Err).os
          << "--lto-known-safe-vtables=: expected symbol to start with _ZTV, "
             "but got "
          << knownSafeName;
    Expected<GlobPattern> pat = GlobPattern::create(knownSafeName);
    if (!pat) {
      SyncStream(ctx.e, DiagLevel::Err).os << "--lto-known-safe-vtables=: "
                                           << toString(pat.takeError());
      continue;
    }
    vtableSymbolsWithNoRTTI.remove_if(
        [&](StringRef s) { return pat->match(s); });
  }

  // Check for unmatched RTTI symbols
  for (StringRef s : vtableSymbolsWithNoRTTI)
    SyncStream(ctx.e, DiagLevel::Msg).os
        << "--lto-validate-all-vtables-have-type-infos: RTTI missing for "
           "vtable _ZTV"
        << s << ", --lto-whole-program-visibility disabled";
  return vtableSymbolsWithNoRTTI.empty();
}
