//===--- WindowsItanium.h - Windows Itanium ToolChains ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H

#include "MSVC.h"

namespace clang {
namespace driver {
namespace tools {
namespace windowsitanium {
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC)
      : Tool("windowsitanium::Linker", "lld-link", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // end namespace windowsitanium
} // end namespace tools

namespace toolchains {

/// The base of the x86-64 and AArch64 Windows toolchains of the Itanium C++
/// ABI: SEH, lld-link, compiler-rt and libc++.
class LLVM_LIBRARY_VISIBILITY WindowsItaniumBase : public WindowsSDKToolChain {
public:
  WindowsItaniumBase(const Driver &D, const llvm::Triple &Triple,
                     const llvm::opt::ArgList &Args);

  bool HasNativeLLVMSupport() const override { return true; }
  bool isPICDefault() const override { return true; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return true; }
  UnwindTableLevel
  getDefaultUnwindTableLevel(const llvm::opt::ArgList &Args) const override {
    return UnwindTableLevel::Asynchronous;
  }
  llvm::ExceptionHandling
  GetExceptionModel(const llvm::opt::ArgList &Args) const override {
    return llvm::ExceptionHandling::WinEH;
  }

  llvm::codegenoptions::DebugInfoFormat getDefaultDebugFormat() const override {
    return llvm::codegenoptions::DIF_CodeView;
  }
  llvm::DebuggerKind getDefaultDebuggerTuning() const override {
    return llvm::DebuggerKind::Default;
  }
  unsigned GetDefaultDwarfVersion() const override { return 4; }

  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return ToolChain::RLT_CompilerRT;
  }
  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return ToolChain::CST_Libcxx;
  }

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, BoundArch BA,
                Action::OffloadKind DeviceOffloadKind) const override;
  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;
  void addClangWarningOptions(llvm::opt::ArgStringList &CC1Args) const override;
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  /// Adds the C library and the system libraries, as default libraries.
  virtual void addSystemLibArgs(const llvm::opt::ArgList &Args,
                                llvm::opt::ArgStringList &CmdArgs) const = 0;

protected:
  Tool *buildLinker() const override;
};

/// Windows Itanium: the Universal CRT and the Windows SDK, without Visual C++.
class LLVM_LIBRARY_VISIBILITY WindowsItaniumToolChain
    : public WindowsItaniumBase {
public:
  WindowsItaniumToolChain(const Driver &D, const llvm::Triple &Triple,
                          const llvm::opt::ArgList &Args);

  UnwindLibType GetUnwindLibType(const llvm::opt::ArgList &Args) const override;
  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;
  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;
  void addSystemLibArgs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
