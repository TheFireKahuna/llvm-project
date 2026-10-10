//===- TargetExecutionUtilsTest.cpp - Tests for TargetExecutionUtils ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

#ifdef _MSC_VER
#include "llvm/Support/Windows/WindowsSupport.h"
#include <stdio.h>
#endif

using namespace llvm;
using namespace llvm::orc;

namespace {

#ifdef _MSC_VER
using SnprintfType = int (*)(char *, size_t, const char *, ...);

// No module exports the Universal CRT's formatted I/O, which its headers
// define inline. Code compiled at run time finds the program's definitions,
// which use its streams, and the search loads no other C runtime. Another test
// may have loaded msvcrt.dll, which exports some of these names and is
// searched first, so the checks use names that msvcrt.dll does not export.
TEST(TargetExecutionUtilsTest, ProcessFindsUCRTFormattedIO) {
  registerInlineCRTFunctions();
  registerInlineCRTFunctions();

  bool HadMSVCRT = GetModuleHandleW(L"msvcrt.dll");
  std::string Err;
  auto DL = sys::DynamicLibrary::getPermanentLibrary(nullptr, &Err);
  ASSERT_TRUE(DL.isValid()) << Err;
  EXPECT_EQ(DL.getAddressOfSymbol("snprintf"), (void *)&snprintf);
  EXPECT_EQ(sys::DynamicLibrary::SearchForAddressOfSymbol("snprintf"),
            (void *)&snprintf);
  EXPECT_NE(DL.getAddressOfSymbol("printf"), nullptr);
  EXPECT_NE(DL.getAddressOfSymbol("_vfwscanf_l"), nullptr);
  EXPECT_NE(DL.getAddressOfSymbol("_vcscanf_s"), nullptr);

  auto Snprintf =
      reinterpret_cast<SnprintfType>(DL.getAddressOfSymbol("snprintf"));
  ASSERT_NE(Snprintf, nullptr);
  char Buffer[16];
  EXPECT_EQ(Snprintf(Buffer, sizeof(Buffer), "%d %s", 42, "x"), 4);
  EXPECT_STREQ(Buffer, "42 x");

  EXPECT_EQ(DL.getAddressOfSymbol("NoSuchFormattedIOFunction"), nullptr);
  if (!HadMSVCRT)
    EXPECT_EQ(GetModuleHandleW(L"msvcrt.dll"), nullptr);
}

TEST(TargetExecutionUtilsTest, ProcessSearchGeneratorRegisters) {
  sys::DynamicLibrary::setProcessSymbolFallback(nullptr);
  auto G = DynamicLibrarySearchGenerator::GetForCurrentProcess('\0');
  ASSERT_THAT_EXPECTED(G, Succeeded());
  EXPECT_EQ(sys::DynamicLibrary::SearchForAddressOfSymbol("snprintf"),
            (void *)&snprintf);
}
#endif

} // namespace
