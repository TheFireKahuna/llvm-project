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

/// NT-POSIX: POSIX on the NT kernel, with llvm-libc as the C library and
/// neither the Universal CRT nor the Windows SDK.
class LLVM_LIBRARY_VISIBILITY NTPOSIXToolChain
    : public WindowsItaniumBaseToolChain {
public:
  NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                   const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, BoundArch BA,
                Action::OffloadKind DeviceOffloadKind) const override;

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;
  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  void addStartFiles(const llvm::opt::ArgList &Args,
                     llvm::opt::ArgStringList &CmdArgs,
                     bool IsDLL) const override;
  void addUnwindLibArgs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  void addSystemLibArgs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;
  void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

private:
  /// Adds a file of the C runtime or the static runtimes, or diagnoses its
  /// absence.
  void addRequiredFile(const llvm::opt::ArgList &Args,
                       llvm::opt::ArgStringList &CmdArgs, const char *Name,
                       bool DefaultLib) const;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NTPOSIX_H
