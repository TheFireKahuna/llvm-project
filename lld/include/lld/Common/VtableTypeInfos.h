//===- VtableTypeInfos.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_VTABLE_TYPE_INFOS_H
#define LLD_COMMON_VTABLE_TYPE_INFOS_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

namespace lld {
class CommonLinkerContext;

// The check of --lto-validate-all-vtables-have-type-infos: whole program
// visibility is safe only if each vtable defined outside the bitcode has a
// type info, by which LTO sees that its class is not confined to the bitcode.
class VtableTypeInfoCheck {
public:
  // Takes the name of a symbol defined in a native object or a shared library.
  void addSymbol(StringRef name) {
    if (name.consume_front("_ZTI"))
      typeInfoSymbols.insert(name);
    else if (name.consume_front("_ZTV"))
      vtableSymbols.insert(name);
  }

  // Reports each vtable that has no type info and matches none of the
  // --lto-known-safe-vtables patterns. Returns true if there is none.
  bool run(CommonLinkerContext &ctx, ArrayRef<StringRef> knownSafeVtables);

private:
  llvm::DenseSet<StringRef> typeInfoSymbols;
  llvm::SmallSetVector<StringRef, 0> vtableSymbols;
};
} // namespace lld

#endif
