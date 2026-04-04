//===--- WindowsItanium.h - Windows Itanium ToolChain -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows Itanium toolchain: Itanium C++ ABI on Windows with UCRT/Win32.
/// Uses lld-link.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H

#include "WindowsItaniumBase.h"
#include "llvm/WindowsDriver/MSVCPaths.h"
#include <optional>

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
                    const llvm::opt::ArgList &Args,
                    const char *LinkingOutput) const override;
};

} // namespace windowsitanium
} // namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY WindowsItaniumToolChain
    : public WindowsItaniumBaseToolChain {
public:
  WindowsItaniumToolChain(const Driver &D, const llvm::Triple &Triple,
                          const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
                Action::OffloadKind DeviceOffloadKind) const override;

  RuntimeLibType GetDefaultRuntimeLibType() const override;

  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                 llvm::opt::ArgStringList &CC1Args) const override;

  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args,
                             Action::OffloadKind DeviceOffloadKind) const override;

  void printVerboseInfo(raw_ostream &OS) const override;

  bool useUniversalCRT() const;

  bool getWindowsSDKLibraryPath(const llvm::opt::ArgList &Args,
                                std::string &Path) const;

  bool getUniversalCRTLibraryPath(const llvm::opt::ArgList &Args,
                                  std::string &Path) const;

  bool FoundWindowsSDK() const { return !WindowsSDKDir.empty(); }

protected:
  Tool *buildLinker() const override;

private:
  std::string VCToolChainPath;
  llvm::ToolsetLayout VSLayout = llvm::ToolsetLayout::OlderVS;

  std::optional<llvm::StringRef> WinSdkDir;
  std::optional<llvm::StringRef> WinSdkVersion;
  std::optional<llvm::StringRef> WinSysRoot;

  std::string WindowsSDKDir;
  int WindowsSDKMajor = 0;
  std::string WindowsSDKIncludeVersion;
  std::string WindowsSDKLibVersion;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
