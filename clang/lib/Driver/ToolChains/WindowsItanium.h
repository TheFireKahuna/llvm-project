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

/// Windows Itanium: the Itanium C++ ABI on the Universal CRT and the Windows
/// SDK. Visual C++ is neither looked for nor used.
class LLVM_LIBRARY_VISIBILITY WindowsItaniumToolChain
    : public WindowsItaniumBaseToolChain {
public:
  WindowsItaniumToolChain(const Driver &D, const llvm::Triple &Triple,
                          const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, BoundArch BA,
                Action::OffloadKind DeviceOffloadKind) const override;

  VersionTuple
  computeMSVCVersion(const Driver *D,
                     const llvm::opt::ArgList &Args) const override;

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;
  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;

  void addSystemLibraryDirs(const llvm::opt::ArgList &Args,
                            std::vector<std::string> &LibDirs) const override;
  void addSystemLibArgs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

private:
  std::optional<llvm::StringRef> WinSdkDir, WinSdkVersion, WinSysRoot;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
