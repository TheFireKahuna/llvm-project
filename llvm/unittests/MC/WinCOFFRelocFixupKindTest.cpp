//===- WinCOFFRelocFixupKindTest.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/ADT/DenseSet.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

// Every relocation type of a machine, 0x00 to 0xFF, and every type from 0x8000
// up has a literal fixup kind of its own, which the object writer reads back.
TEST(WinCOFFRelocFixupKindTest, Injective) {
  DenseSet<unsigned> Kinds;
  for (unsigned Base : {0x0000u, 0x8000u})
    for (unsigned Type = Base; Type != Base + 0x100; ++Type) {
      MCFixupKind Kind = getCOFFRelocFixupKind(Type);
      EXPECT_TRUE(mc::isRelocRelocation(Kind)) << Type;
      EXPECT_TRUE(Kinds.insert(Kind).second) << Type;
      EXPECT_EQ(getCOFFRelocType(Kind), Type);
    }
}

// The literal kind of no ELF type that .reloc names on x86-64 or AArch64 is
// the kind of a type from 0x8000 up, which code shared with ELF compares with.
TEST(WinCOFFRelocFixupKindTest, DisjointFromELF) {
  DenseSet<unsigned> Kinds;
  for (unsigned Type = 0x8000; Type != 0x8100; ++Type)
    Kinds.insert(getCOFFRelocFixupKind(Type));
#define ELF_RELOC(Name, Value)                                                 \
  EXPECT_FALSE(Kinds.contains(FirstLiteralRelocationKind + Value)) << #Name;
#include "llvm/BinaryFormat/ELFRelocs/AArch64.def"
#include "llvm/BinaryFormat/ELFRelocs/x86_64.def"
#undef ELF_RELOC
}

} // namespace
