//===--- WindowsItanium.h - Windows Itanium ToolChain -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H

#include "WindowsItaniumBase.h"
#include <optional>

namespace clang {
namespace driver {
namespace toolchains {

/// Windows Itanium: the Itanium C++ ABI with the UCRT and Win32. Only the
/// Windows SDK and the UCRT are located; Visual C++ is never used.
class LLVM_LIBRARY_VISIBILITY WindowsItaniumToolChain
    : public WindowsItaniumBaseToolChain {
public:
  WindowsItaniumToolChain(const Driver &D, const llvm::Triple &Triple,
                          const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
                Action::OffloadKind DeviceOffloadKind) const override;

  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args,
                        Action::OffloadKind DeviceOffloadKind) const override;

  bool isCETCompatible() const override {
    return getArch() == llvm::Triple::x86_64;
  }

  VersionTuple
  computeMSVCVersion(const Driver *D,
                     const llvm::opt::ArgList &Args) const override;

  StringRef
  getExecutableEntryPoint(const llvm::opt::ArgList &Args) const override;
  void addSystemLinkArgs(const llvm::opt::ArgList &Args,
                         llvm::opt::ArgStringList &CmdArgs,
                         bool IsDLL) const override;
  bool addLibCArgs(const llvm::opt::ArgList &Args,
                   llvm::opt::ArgStringList &CmdArgs) const override;
  void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

private:
  bool getWindowsSDKLibraryPath(const llvm::opt::ArgList &Args,
                                std::string &Path) const;
  bool getUniversalCRTLibraryPath(const llvm::opt::ArgList &Args,
                                  std::string &Path) const;

  std::optional<llvm::StringRef> WinSdkDir;
  std::optional<llvm::StringRef> WinSdkVersion;
  std::optional<llvm::StringRef> WinSysRoot;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
