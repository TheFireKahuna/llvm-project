//===-- llvm/unittest/Support/SignalsTest.cpp - Signals unit tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains unit tests for Signals.cpp and Signals.inc.
///
//===----------------------------------------------------------------------===//

#include "llvm/Support/Signals.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Config/config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#ifdef LLVM_RUNTIME_WIN32
#include "llvm/Support/Windows/WindowsSupport.h"
#endif

using namespace llvm;
using namespace llvm::sys;
using testing::MatchesRegex;
using testing::Not;

#define TAG_BEGIN "\\{\\{\\{"
#define TAG_END "\\}\\}\\}"
// %p in the Symbolizer Markup Format spec
#define P_REGEX "(0+|0x[0-9a-fA-F]+)"
// %i in the Symbolizer Markup Format spec
#define I_REGEX "(0x[0-9a-fA-F]+|0[0-7]+|[0-9]+)"

#if defined(HAVE_BACKTRACE) && ENABLE_BACKTRACES &&                            \
    (defined(__linux__) || defined(__FreeBSD__) ||                             \
     defined(__FreeBSD_kernel__) || defined(__NetBSD__))
// Test relies on the binary this test is linked into having a GNU build ID
// note, which is not universally enabled by default (even when using Clang).
// Disable until we can reliably detect whether this is the case and skip it if
// not. See https://github.com/llvm/llvm-project/issues/168891.
TEST(SignalsTest, PrintsSymbolizerMarkup) {
  scope_exit Exit([]() { unsetenv("LLVM_ENABLE_SYMBOLIZER_MARKUP"); });
  setenv("LLVM_ENABLE_SYMBOLIZER_MARKUP", "1", 1);
  std::string Res;
  raw_string_ostream RawStream(Res);
  PrintStackTrace(RawStream);
  if (!StringRef(Res).contains("SupportTests"))
    GTEST_SKIP() << "build ID could not be found for the main binary";
  EXPECT_THAT(Res, MatchesRegex(TAG_BEGIN "reset" TAG_END ".*"));
  // Module line for main binary
  EXPECT_THAT(Res,
              MatchesRegex(".*" TAG_BEGIN
                           "module:0:[^:]*SupportTests:elf:[0-9a-f]+" TAG_END
                           ".*"));
  // Text segment for main binary
  EXPECT_THAT(Res, MatchesRegex(".*" TAG_BEGIN "mmap:" P_REGEX ":" I_REGEX
                                ":load:0:rx:" P_REGEX TAG_END ".*"));
  // Backtrace line
  EXPECT_THAT(Res, MatchesRegex(".*" TAG_BEGIN "bt:0:" P_REGEX ".*"));
}

TEST(SignalsTest, SymbolizerMarkupDisabled) {
  scope_exit Exit([]() { unsetenv("LLVM_DISABLE_SYMBOLIZATION"); });
  setenv("LLVM_DISABLE_SYMBOLIZATION", "1", 1);
  std::string Res;
  raw_string_ostream RawStream(Res);
  PrintStackTrace(RawStream);
  EXPECT_THAT(Res, Not(MatchesRegex(TAG_BEGIN "reset" TAG_END ".*")));
}

#endif // defined(HAVE_BACKTRACE) && ...

#ifdef LLVM_RUNTIME_WIN32
extern const char *TestMainArgv0;

static int SignalsTestAnchor;

// The test main registers the crash handlers. Run the check in a child process,
// whose only work after that is this test.
TEST(SignalsTest, LoadsDbgHelpOnFirstStackTrace) {
  if (getenv("LLVM_SIGNALS_TEST_CHILD")) {
    if (::GetModuleHandleW(L"dbghelp.dll"))
      exit(1);
    std::string Res;
    raw_string_ostream RawStream(Res);
    PrintStackTrace(RawStream);
    if (!::GetModuleHandleW(L"dbghelp.dll") || Res.empty())
      exit(2);
    exit(0);
  }

  std::string Exe = fs::getMainExecutable(TestMainArgv0, &SignalsTestAnchor);
  StringRef Args[] = {
      Exe, "--gtest_filter=SignalsTest.LoadsDbgHelpOnFirstStackTrace"};
  ASSERT_TRUE(::SetEnvironmentVariableW(L"LLVM_SIGNALS_TEST_CHILD", L"1"));
  scope_exit Exit(
      []() { ::SetEnvironmentVariableW(L"LLVM_SIGNALS_TEST_CHILD", nullptr); });
  std::string Error;
  bool ExecutionFailed;
  int RC = ExecuteAndWait(Exe, Args, std::nullopt, {}, /*SecondsToWait=*/60,
                          /*MemoryLimit=*/0, &Error, &ExecutionFailed);
  EXPECT_FALSE(ExecutionFailed) << Error;
  EXPECT_EQ(0, RC);
}
#endif // LLVM_RUNTIME_WIN32
