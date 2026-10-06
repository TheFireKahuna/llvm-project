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
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Testing/Support/Error.h"
#include "llvm/Testing/Support/SupportHelpers.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace object;

// Extra members follow the import members, with their names and contents.
TEST(COFFImportFileTest, ExtraMembers) {
  unittest::TempDir Dir("coff-import-file", /*Unique=*/true);
  std::string Path(Dir.path("test.lib"));

  COFFShortExport E;
  E.Name = "f";
  std::vector<NewArchiveMember> Extra;
  Extra.emplace_back(MemoryBufferRef("facts", "facts.obj"));
  ASSERT_THAT_ERROR(writeImportLibrary("test.dll", Path, {E},
                                       COFF::IMAGE_FILE_MACHINE_AMD64,
                                       /*MinGW=*/false, {}, Extra),
                    Succeeded());

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
