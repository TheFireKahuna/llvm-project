//===- llvm/unittest/Support/DynamicLibrary/DynamicLibraryTest.cpp --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"

#include "PipSqueak.h"

// FIXME: Missing globals/DSO https://github.com/llvm/llvm-project/issues/57206.
#if !LLVM_HWADDRESS_SANITIZER_BUILD

using namespace llvm;
using namespace llvm::sys;

std::string LibPath(const std::string Name = "PipSqueak") {
  const auto &Argvs = testing::internal::GetArgvs();
  const char *Argv0 =
      Argvs.size() > 0 ? Argvs[0].c_str() : "DynamicLibraryTests";
  void *Ptr = (void*)(intptr_t)TestA;
  std::string Path = fs::getMainExecutable(Argv0, Ptr);
  llvm::SmallString<256> Buf(path::parent_path(Path));
  path::append(Buf, (Name + LLVM_PLUGIN_EXT).c_str());
  return std::string(Buf.str());
}

#if defined(_WIN32) || defined(HAVE_DLOPEN)

typedef void (*SetStrings)(std::string &GStr, std::string &LStr);
typedef void (*TestOrder)(std::vector<std::string> &V);
typedef const char *(*GetString)();

template <class T> static T FuncPtr(void *Ptr) {
  union {
    T F;
    void *P;
  } Tmp;
  Tmp.P = Ptr;
  return Tmp.F;
}
template <class T> static void* PtrFunc(T *Func) {
  union {
    T *F;
    void *P;
  } Tmp;
  Tmp.F = Func;
  return Tmp.P;
}

static const char *OverloadTestA() { return "OverloadCall"; }

std::string StdString(const char *Ptr) { return Ptr ? Ptr : ""; }

TEST(DynamicLibrary, Overload)
#ifdef __clang__
LLVM_NO_SANITIZE("cfi-icall")
#endif
{
  {
    std::string Err;
    DynamicLibrary DL =
        DynamicLibrary::getPermanentLibrary(LibPath().c_str(), &Err);
    EXPECT_TRUE(DL.isValid());
    EXPECT_TRUE(Err.empty());

    GetString GS = FuncPtr<GetString>(DL.getAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_NE(GS, &TestA);
    EXPECT_EQ(StdString(GS()), "LibCall");

    GS = FuncPtr<GetString>(DynamicLibrary::SearchForAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_NE(GS, &TestA);
    EXPECT_EQ(StdString(GS()), "LibCall");

    DL = DynamicLibrary::getPermanentLibrary(nullptr, &Err);
    EXPECT_TRUE(DL.isValid());
    EXPECT_TRUE(Err.empty());

    // Test overloading local symbols does not occur by default
    GS = FuncPtr<GetString>(DynamicLibrary::SearchForAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_EQ(GS, &TestA);
    EXPECT_EQ(StdString(GS()), "ProcessCall");

    GS = FuncPtr<GetString>(DL.getAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_EQ(GS, &TestA);
    EXPECT_EQ(StdString(GS()), "ProcessCall");

    // Test overloading by forcing library priority when searching for a symbol
    DynamicLibrary::SearchOrder = DynamicLibrary::SO_LoadedFirst;
    GS = FuncPtr<GetString>(DynamicLibrary::SearchForAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_NE(GS, &TestA);
    EXPECT_EQ(StdString(GS()), "LibCall");

    DynamicLibrary::AddSymbol("TestA", PtrFunc(&OverloadTestA));
    GS = FuncPtr<GetString>(DL.getAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_NE(GS, &OverloadTestA);

    GS = FuncPtr<GetString>(DynamicLibrary::SearchForAddressOfSymbol("TestA"));
    EXPECT_NE(GS, nullptr);
    EXPECT_EQ(GS, &OverloadTestA);
    EXPECT_EQ(StdString(GS()), "OverloadCall");
  }
}

static int FallbackTarget;

static void *ProcessFallback(const char *SymbolName) {
  return StringRef(SymbolName) == "NoModuleDefinesThis" ? &FallbackTarget
                                                        : nullptr;
}

// A search of the program handle ends with the fallback, which a search of a
// library handle never consults.
TEST(DynamicLibrary, ProcessSymbolFallback) {
  std::string Err;
  DynamicLibrary DL = DynamicLibrary::getPermanentLibrary(nullptr, &Err);
  ASSERT_TRUE(DL.isValid()) << Err;
  EXPECT_EQ(DL.getAddressOfSymbol("NoModuleDefinesThis"), nullptr);

  DynamicLibrary::setProcessSymbolFallback(ProcessFallback);
  EXPECT_EQ(DL.getAddressOfSymbol("NoModuleDefinesThis"), &FallbackTarget);
  EXPECT_EQ(DynamicLibrary::SearchForAddressOfSymbol("NoModuleDefinesThis"),
            &FallbackTarget);
  EXPECT_EQ(DL.getAddressOfSymbol("NorThis"), nullptr);

  DynamicLibrary Lib = DynamicLibrary::getLibrary(LibPath().c_str(), &Err);
  ASSERT_TRUE(Lib.isValid()) << Err;
  EXPECT_EQ(Lib.getAddressOfSymbol("NoModuleDefinesThis"), nullptr);
  DynamicLibrary::closeLibrary(Lib);

  DynamicLibrary::setProcessSymbolFallback(nullptr);
  EXPECT_EQ(DL.getAddressOfSymbol("NoModuleDefinesThis"), nullptr);
}

#else

TEST(DynamicLibrary, Unsupported) {
  std::string Err;
  DynamicLibrary DL =
      DynamicLibrary::getPermanentLibrary(LibPath().c_str(), &Err);
  EXPECT_FALSE(DL.isValid());
  EXPECT_EQ(Err, "dlopen() not supported on this platform");
}

#endif

#endif
