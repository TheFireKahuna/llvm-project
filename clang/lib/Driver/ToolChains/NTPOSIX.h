//===--- NTPOSIX.h - NT-POSIX ToolChain ----------------------*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NT-POSIX toolchain: Itanium C++ ABI on the Windows NT kernel with
/// llvm-libc providing POSIX semantics. No Win32/UCRT dependency.
/// Uses lld-link, COFF/PE.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H

#include "WindowsItaniumBase.h"

namespace clang {
namespace driver {
namespace tools {
namespace ntposix {

class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC)
      : Tool("ntposix::Linker", "lld-link", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &Args,
                    const char *LinkingOutput) const override;
};

} // namespace ntposix
} // namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY NTPOSIXToolChain
    : public WindowsItaniumBaseToolChain {
public:
  NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                   const llvm::opt::ArgList &Args);

  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return ToolChain::RLT_CompilerRT;
  }

  void AddClangSystemIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  void addClangTargetOptions(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args,
      Action::OffloadKind DeviceOffloadKind) const override;

protected:
  Tool *buildLinker() const override;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H
