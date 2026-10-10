//===- COFFImportFileTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/COFFImportFile.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Testing/Support/SupportHelpers.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace object;

// Checks that the import library at Path ends with the member facts.obj.
static void checkExtraMember(StringRef Path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buf = MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Buf));
  Expected<std::unique_ptr<Archive>> A = Archive::create(**Buf);
  ASSERT_THAT_EXPECTED(A, Succeeded());

  Error Err = Error::success();
  std::vector<std::pair<std::string, std::string>> Members;
  for (const Archive::Child &C : (*A)->children(Err)) {
    Expected<StringRef> Name = C.getName();
    Expected<StringRef> Contents = C.getBuffer();
    ASSERT_THAT_EXPECTED(Name, Succeeded());
    ASSERT_THAT_EXPECTED(Contents, Succeeded());
    Members.emplace_back(Name->str(), Contents->str());
  }
  ASSERT_THAT_ERROR(std::move(Err), Succeeded());
  // The import descriptor, the null import descriptor, the null thunk and f.
  ASSERT_EQ(Members.size(), 5u);
  EXPECT_EQ(Members.back().first, "facts.obj");
  EXPECT_EQ(Members.back().second, "facts");
}

// Extra members follow the import members, with their names and contents,
// and stay the caller's, which can write them again.
TEST(COFFImportFileTest, ExtraMembers) {
  unittest::TempDir Dir("coff-import-file", /*Unique=*/true);
  std::string Path(Dir.path("test.lib"));
  std::string Path2(Dir.path("test2.lib"));

  COFFShortExport E;
  E.Name = "f";
  std::vector<NewArchiveMember> Extra;
  Extra.emplace_back(MemoryBufferRef("facts", "facts.obj"));
  ASSERT_THAT_ERROR(writeImportLibrary("test.dll", Path, {E},
                                       COFF::IMAGE_FILE_MACHINE_AMD64,
                                       /*MinGW=*/false, {}, Extra),
                    Succeeded());
  ASSERT_THAT_ERROR(writeImportLibrary("test.dll", Path2, {E},
                                       COFF::IMAGE_FILE_MACHINE_AMD64,
                                       /*MinGW=*/false, {}, Extra),
                    Succeeded());
  ASSERT_TRUE(Extra.front().Buf);

  for (const std::string &P : {Path, Path2})
    checkExtraMember(P);
}

// A member of linker facts is an object whose one section, which the linker
// removes, holds the facts, and whose one symbol is absolute.
TEST(COFFImportFileTest, LinkerFacts) {
  std::vector<uint8_t> Buffer;
  NewArchiveMember M = createLinkerFacts(
      "test.dll", COFF::IMAGE_FILE_MACHINE_AMD64, ".llvm_link_records",
      arrayRefFromStringRef("facts"), "facts$test.dll", Buffer);
  EXPECT_EQ(M.MemberName, "test.dll");

  Expected<std::unique_ptr<COFFObjectFile>> Obj =
      COFFObjectFile::create(M.Buf->getMemBufferRef());
  ASSERT_THAT_EXPECTED(Obj, Succeeded());
  EXPECT_EQ((*Obj)->getMachine(), COFF::IMAGE_FILE_MACHINE_AMD64);

  ASSERT_EQ((*Obj)->getNumberOfSections(), 1u);
  Expected<const coff_section *> Sec = (*Obj)->getSection(1);
  ASSERT_THAT_EXPECTED(Sec, Succeeded());
  Expected<StringRef> SecName = (*Obj)->getSectionName(*Sec);
  ASSERT_THAT_EXPECTED(SecName, Succeeded());
  EXPECT_EQ(*SecName, ".llvm_link_records");
  EXPECT_EQ((*Sec)->Characteristics, uint32_t(COFF::IMAGE_SCN_LNK_REMOVE));
  ArrayRef<uint8_t> Contents;
  ASSERT_THAT_ERROR((*Obj)->getSectionContents(*Sec, Contents), Succeeded());
  EXPECT_EQ(toStringRef(Contents), "facts");

  ASSERT_EQ((*Obj)->getNumberOfSymbols(), 1u);
  Expected<COFFSymbolRef> Sym = (*Obj)->getSymbol(0);
  ASSERT_THAT_EXPECTED(Sym, Succeeded());
  Expected<StringRef> SymName = (*Obj)->getSymbolName(*Sym);
  ASSERT_THAT_EXPECTED(SymName, Succeeded());
  EXPECT_EQ(*SymName, "facts$test.dll");
  EXPECT_TRUE(Sym->isAbsolute());
  EXPECT_TRUE(Sym->isExternal());
}
