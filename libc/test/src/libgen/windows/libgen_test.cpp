//===-- Unit tests for basename / dirname -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/libgen/basename.h"
#include "src/libgen/dirname.h"
#include "test/UnitTest/Test.h"

TEST(LlvmLibcLibgenTest, PosixBasenameCases) {
  EXPECT_STREQ(LIBC_NAMESPACE::basename(nullptr), ".");

  char empty[] = "";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(empty), ".");

  char usr[] = "usr";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(usr), "usr");

  char usr_slash[] = "usr/";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(usr_slash), "usr");
  EXPECT_STREQ(usr_slash, "usr");

  char root[] = "/";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(root), "/");

  char double_root[] = "//";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(double_root), "//");

  char triple_root[] = "///";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(triple_root), "/");

  char nested[] = "//usr//lib//";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(nested), "lib");

  char dotted[] = "/home/dwc/.";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(dotted), ".");
}

TEST(LlvmLibcLibgenTest, PosixDirnameCases) {
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(nullptr), ".");

  char empty[] = "";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(empty), ".");

  char usr[] = "usr";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(usr), ".");

  char usr_slash[] = "usr/";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(usr_slash), ".");

  char root[] = "/";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(root), "/");

  char double_root[] = "//";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(double_root), "//");

  char triple_root[] = "///";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(triple_root), "/");

  char nested[] = "//usr//lib//";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(nested), "//usr");
  EXPECT_STREQ(nested, "//usr");

  char dotted[] = "/home/.././test";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(dotted), "/home/../.");
  EXPECT_STREQ(dotted, "/home/../.");
}

TEST(LlvmLibcLibgenTest, WindowsSeparatorsAreLexicalDelimiters) {
  char path1[] = "C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(path1), "bar");

  char path2[] = "C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(path2), "C:\\foo");
  EXPECT_STREQ(path2, "C:\\foo");

  char mixed1[] = "C:/foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(mixed1), "bar");

  char mixed2[] = "C:/foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(mixed2), "C:/foo");
  EXPECT_STREQ(mixed2, "C:/foo");
}

TEST(LlvmLibcLibgenTest, WindowsRootsRemainIntact) {
  char drive_root_base[] = "C:\\";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(drive_root_base), "C:\\");

  char drive_root_dir[] = "C:\\";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(drive_root_dir), "C:\\");

  char drive_relative[] = "C:foo";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(drive_relative), "C:foo");

  char drive_relative_dir[] = "C:foo";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(drive_relative_dir), ".");

  char unc_root_base[] = "\\\\server\\share\\";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(unc_root_base), "\\\\server\\share");

  char unc_root_dir[] = "\\\\server\\share\\";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(unc_root_dir), "\\\\server\\share");

  char unc_file[] = "\\\\server\\share\\dir\\file";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(unc_file), "file");

  char unc_file_dir[] = "\\\\server\\share\\dir\\file";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(unc_file_dir),
               "\\\\server\\share\\dir");
}

TEST(LlvmLibcLibgenTest, NtNamespacePrefixesAreHandledLexically) {
  char verbatim1[] = "\\\\?\\C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(verbatim1), "bar");

  char verbatim2[] = "\\\\?\\C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(verbatim2), "\\\\?\\C:\\foo");
  EXPECT_STREQ(verbatim2, "\\\\?\\C:\\foo");

  char ntpref1[] = "\\??\\C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::basename(ntpref1), "bar");

  char ntpref2[] = "\\??\\C:\\foo\\bar";
  EXPECT_STREQ(LIBC_NAMESPACE::dirname(ntpref2), "\\??\\C:\\foo");
  EXPECT_STREQ(ntpref2, "\\??\\C:\\foo");
}
