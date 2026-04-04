//===--- WindowsItaniumBase.h - Shared base for Win+Itanium ---*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared base class for Windows toolchains that use the Itanium C++ ABI,
/// lld-link, COFF/PE, and compiler-rt/libunwind/libc++.
///
/// Concrete personalities:
///   WindowsItaniumToolChain  — UCRT/Win32, optional llvm-libc
///   NTPOSIXToolChain         — NT-POSIX, always llvm-libc
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H

#include "clang/Driver/CudaInstallationDetector.h"
#include "clang/Driver/LazyDetector.h"
#include "clang/Driver/RocmInstallationDetector.h"
#include "clang/Driver/SyclInstallationDetector.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Frontend/Debug/Options.h"

namespace clang {
namespace driver {
namespace toolchains {

class LLVM_LIBRARY_VISIBILITY WindowsItaniumBaseToolChain : public ToolChain {
public:
  WindowsItaniumBaseToolChain(const Driver &D, const llvm::Triple &Triple,
                              const llvm::opt::ArgList &Args);

  bool HasNativeLLVMSupport() const override { return true; }

  UnwindTableLevel
  getDefaultUnwindTableLevel(const llvm::opt::ArgList &Args) const override;

  bool isPICDefault() const override;
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override;
  bool isPICDefaultForced() const override;

  llvm::codegenoptions::DebugInfoFormat getDefaultDebugFormat() const override {
    return llvm::codegenoptions::DIF_CodeView;
  }

  llvm::DebuggerKind getDefaultDebuggerTuning() const override {
    return llvm::DebuggerKind::Default;
  }

  unsigned GetDefaultDwarfVersion() const override { return 4; }

  llvm::ExceptionHandling
  GetExceptionModel(const llvm::opt::ArgList &Args) const override;

  SanitizerMask getSupportedSanitizers() const override;

  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return ToolChain::CST_Libcxx;
  }

  CXXStdlibType
  GetCXXStdlibType(const llvm::opt::ArgList &Args) const override;

  UnwindLibType GetDefaultUnwindLibType() const override {
    return ToolChain::UNW_CompilerRT;
  }

  const char *getDefaultLinker() const override { return "lld-link"; }

  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  void AddCudaIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;
  void AddHIPIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                         llvm::opt::ArgStringList &CC1Args) const override;
  void addSYCLIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;
  void addOffloadRTLibs(unsigned ActiveKinds, const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  void AddRuntimeLibSearchPaths(const llvm::opt::ArgList &Args,
                                llvm::opt::ArgStringList &CmdArgs) const;
  void NormalizeLLDLinkArgs(const llvm::opt::ArgList &Args,
                            llvm::opt::ArgStringList &CmdArgs) const;

  void printVerboseInfo(raw_ostream &OS) const override;

protected:
  void AddSystemIncludeWithSubfolder(const llvm::opt::ArgList &DriverArgs,
                                     llvm::opt::ArgStringList &CC1Args,
                                     const std::string &Folder,
                                     const Twine &Subfolder1,
                                     const Twine &Subfolder2 = "",
                                     const Twine &Subfolder3 = "") const;

  LazyDetector<CudaInstallationDetector> CudaInstallation;
  LazyDetector<RocmInstallationDetector> RocmInstallation;
  LazyDetector<SYCLInstallationDetector> SYCLInstallation;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H
