//===- llvm/unittest/Support/COMTest.cpp - COM initialization tests -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/COM.h"
#include "llvm/Config/llvm-config.h"
#include "gtest/gtest.h"

#ifdef LLVM_RUNTIME_WIN32
#include "llvm/Support/Windows/WindowsSupport.h"
#include <objbase.h>
#include <thread>

using namespace llvm;
using namespace llvm::sys;

namespace {

// A thread that is already in a single-threaded apartment keeps it after an
// InitializeCOMRAII asking for the other kind has come and gone.
TEST(COMTest, KeepsExistingApartment) {
  std::thread([] {
    ASSERT_EQ(S_OK, ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    {
      InitializeCOMRAII COM(COMThreadingMode::MultiThreaded);
    }
    // S_FALSE means the thread was still initialized.
    EXPECT_EQ(S_FALSE, ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    ::CoUninitialize();
    ::CoUninitialize();
  }).join();
}

} // namespace
#endif // LLVM_RUNTIME_WIN32
