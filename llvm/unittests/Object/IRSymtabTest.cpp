//===- IRSymtabTest.cpp - IR symbol table requirements
//---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/IRSymtab.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::irsymtab;

namespace {
struct RequirementTable {
  storage::Header Header{};
  storage::Symbol Symbols[2]{};
  storage::COFFABIRequirement Requirements[2]{};

  RequirementTable() {
    Header.Version = storage::Header::kCurrentVersion;
    Header.Symbols.Offset = offsetof(RequirementTable, Symbols);
    Header.Symbols.Size = 2;
    Header.COFFABIRequirements.Offset =
        offsetof(RequirementTable, Requirements);
    Header.COFFABIRequirements.Size = 2;
    Requirements[0].SymbolIndex = 0;
    Requirements[0].Contract.Offset = 0;
    Requirements[0].Contract.Size = 1;
    Requirements[1].SymbolIndex = 1;
    Requirements[1].Contract.Offset = 1;
    Requirements[1].Contract.Size = 1;
  }

  Expected<std::vector<COFFABIRequirement>> read() const {
    return Reader(
               StringRef(reinterpret_cast<const char *>(this), sizeof(*this)),
               "AB")
        .getCOFFABIRequirements();
  }
};

TEST(IRSymtabTest, SparseABIRequirements) {
  RequirementTable Table;
  auto Rows = Table.read();
  ASSERT_TRUE(bool(Rows)) << toString(Rows.takeError());
  ASSERT_EQ(2U, Rows->size());
  EXPECT_EQ(0U, (*Rows)[0].SymbolIndex);
  EXPECT_EQ("A", (*Rows)[0].Contract);
  EXPECT_EQ(1U, (*Rows)[1].SymbolIndex);
  EXPECT_EQ("B", (*Rows)[1].Contract);
  Table.Requirements[1].SymbolIndex = 0;
  Rows = Table.read();
  ASSERT_TRUE(bool(Rows)) << toString(Rows.takeError());
  EXPECT_EQ(0U, (*Rows)[1].SymbolIndex);
}

TEST(IRSymtabTest, RejectMalformedABIRequirements) {
  for (unsigned Case = 0; Case != 8; ++Case) {
    RequirementTable Table;
    switch (Case) {
    case 0:
      Table.Header.COFFABIRequirements.Offset = UINT32_MAX;
      break;
    case 1:
      Table.Header.COFFABIRequirements.Size = UINT32_MAX;
      break;
    case 2:
      Table.Requirements[0].SymbolIndex = 2;
      break;
    case 3:
      Table.Requirements[0].SymbolIndex = 1;
      Table.Requirements[1].SymbolIndex = 0;
      break;
    case 4:
      Table.Requirements[0].Contract.Offset = UINT32_MAX;
      break;
    case 5:
      Table.Requirements[0].Contract.Size = UINT32_MAX;
      break;
    case 6:
      Table.Requirements[0].Contract.Size = 0;
      break;
    case 7:
      Table.Header.COFFABIRequirements.Offset = sizeof(Table) - 1;
      break;
    }
    auto Rows = Table.read();
    EXPECT_FALSE(bool(Rows)) << Case;
    consumeError(Rows.takeError());
  }
}
} // namespace
