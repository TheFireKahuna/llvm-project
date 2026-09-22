//===--- NTPOSIX.h - NT-POSIX ToolChain -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H

#include "WindowsItaniumBase.h"

namespace clang {
namespace driver {
namespace toolchains {

/// NT-POSIX: the NT kernel with llvm-libc as the C library, and no Win32
/// runtime or Windows SDK.
class LLVM_LIBRARY_VISIBILITY NTPOSIXToolChain
    : public WindowsItaniumBaseToolChain {
public:
  NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                   const llvm::opt::ArgList &Args);

  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args,
                        Action::OffloadKind DeviceOffloadKind) const override;

  bool addStartFiles(const llvm::opt::ArgList &Args,
                     llvm::opt::ArgStringList &CmdArgs,
                     bool IsDLL) const override;
  bool addLibCArgs(const llvm::opt::ArgList &Args,
                   llvm::opt::ArgStringList &CmdArgs) const override;
  void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;
  bool addPostInputLibs(const llvm::opt::ArgList &Args,
                        const InputInfoList &Inputs,
                        llvm::opt::ArgStringList &CmdArgs) const override;

private:
  /// Adds a file of the C runtime, or diagnoses its absence.
  bool addRequiredFile(const llvm::opt::ArgList &Args,
                       llvm::opt::ArgStringList &CmdArgs, const char *Name,
                       bool DefaultLib = false) const;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H
