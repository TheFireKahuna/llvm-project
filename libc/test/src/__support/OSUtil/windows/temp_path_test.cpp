//===-- Windows temp-path helper tests ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/temp_path.h"
#include "test/UnitTest/Test.h"

namespace {

const WCHAR *w(const char16_t *s) {
  return reinterpret_cast<const WCHAR *>(s);
}

constexpr WCHAR wc(char16_t ch) { return static_cast<WCHAR>(ch); }

} // namespace

TEST(LlvmLibcWindowsTempPathTest, PrefersTMP) {
  constexpr WCHAR ENV[] = {
      u'T', u'M', u'P', u'=', u'C', u':', u'\\', u't', u'm', u'p', u'\0',
      u'T', u'E', u'M', u'P', u'=', u'C', u':', u'\\', u'o', u't', u'h', u'e',
      u'r', u'\0', u'\0'};
  WCHAR path[64] = {};
  size_t len = LIBC_NAMESPACE::windows::get_temp_path_from_env_block(
      ENV, w(u"C:\\Windows"), path, 64);
  ASSERT_EQ(len, static_cast<size_t>(7));
  EXPECT_EQ(path[0], wc(u'C'));
  EXPECT_EQ(path[1], wc(u':'));
  EXPECT_EQ(path[2], wc(u'\\'));
  EXPECT_EQ(path[3], wc(u't'));
  EXPECT_EQ(path[4], wc(u'm'));
  EXPECT_EQ(path[5], wc(u'p'));
  EXPECT_EQ(path[6], wc(u'\\'));
  EXPECT_EQ(path[7], wc(u'\0'));
}

TEST(LlvmLibcWindowsTempPathTest, FallsBackToTEMPWhenTMPEmpty) {
  constexpr WCHAR ENV[] = {
      u'T', u'M', u'P', u'=', u'\0', u'T', u'E', u'M', u'P', u'=', u'D', u':',
      u'\\', u's', u'c', u'r', u'a', u't', u'c', u'h', u'\0', u'\0'};
  WCHAR path[64] = {};
  size_t len = LIBC_NAMESPACE::windows::get_temp_path_from_env_block(
      ENV, w(u"C:\\Windows"), path, 64);
  ASSERT_EQ(len, static_cast<size_t>(11));
  EXPECT_EQ(path[10], wc(u'\\'));
}

TEST(LlvmLibcWindowsTempPathTest, MatchesEnvNamesCaseInsensitively) {
  constexpr WCHAR ENV[] = {
      u't', u'm', u'p', u'=', u'D', u':', u'/', u's', u'c', u'r', u'a', u't',
      u'c', u'h', u'\0', u'\0'};
  WCHAR path[64] = {};
  size_t len = LIBC_NAMESPACE::windows::get_temp_path_from_env_block(
      ENV, w(u"C:\\Windows"), path, 64);
  ASSERT_EQ(len, static_cast<size_t>(11));
  EXPECT_EQ(path[0], wc(u'D'));
  EXPECT_EQ(path[2], wc(u'\\'));
  EXPECT_EQ(path[10], wc(u'\\'));
}

TEST(LlvmLibcWindowsTempPathTest, UsesLocalAppDataTempWhenNeeded) {
  constexpr WCHAR ENV[] = {
      u'L', u'O', u'C', u'A', u'L', u'A', u'P', u'P', u'D', u'A', u'T', u'A',
      u'=', u'C', u':', u'\\', u'U', u's', u'e', u'r', u's', u'\\', u'm', u'e',
      u'\\', u'A', u'p', u'p', u'D', u'a', u't', u'a', u'\\', u'L', u'o', u'c',
      u'a', u'l', u'\0', u'\0'};
  WCHAR path[128] = {};
  size_t len = LIBC_NAMESPACE::windows::get_temp_path_from_env_block(
      ENV, w(u"C:\\Windows"), path, 128);
  ASSERT_GT(len, static_cast<size_t>(5));
  EXPECT_EQ(path[len - 1], wc(u'\\'));
  EXPECT_EQ(path[len - 6], wc(u'\\'));
  EXPECT_EQ(path[len - 5], wc(u'T'));
  EXPECT_EQ(path[len - 4], wc(u'e'));
  EXPECT_EQ(path[len - 3], wc(u'm'));
  EXPECT_EQ(path[len - 2], wc(u'p'));
  EXPECT_EQ(path[len - 1], wc(u'\\'));
}

TEST(LlvmLibcWindowsTempPathTest, FallsBackToSystemRootTemp) {
  constexpr WCHAR ENV[] = {u'\0'};
  WCHAR path[64] = {};
  size_t len = LIBC_NAMESPACE::windows::get_temp_path_from_env_block(
      ENV, w(u"C:\\Windows"), path, 64);
  ASSERT_EQ(len, static_cast<size_t>(16));
  EXPECT_EQ(path[0], wc(u'C'));
  EXPECT_EQ(path[3], wc(u'W'));
  EXPECT_EQ(path[9], wc(u's'));
  EXPECT_EQ(path[10], wc(u'\\'));
  EXPECT_EQ(path[11], wc(u'T'));
  EXPECT_EQ(path[15], wc(u'\\'));
}
