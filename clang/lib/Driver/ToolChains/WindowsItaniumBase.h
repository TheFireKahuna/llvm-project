//===--- WindowsItaniumBase.h - Windows Itanium base ToolChain --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H

#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/Frontend/Debug/Options.h"

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

/// The base of the x86-64 and AArch64 Windows toolchains that use the Itanium
/// C++ ABI with SEH, lld-link, compiler-rt, libunwind and libc++.
class LLVM_LIBRARY_VISIBILITY WindowsItaniumBaseToolChain : public ToolChain {
public:
  WindowsItaniumBaseToolChain(const Driver &D, const llvm::Triple &Triple,
                              const llvm::opt::ArgList &Args);

  bool HasNativeLLVMSupport() const override { return true; }

  UnwindTableLevel
  getDefaultUnwindTableLevel(const llvm::opt::ArgList &Args) const override {
    return UnwindTableLevel::Asynchronous;
  }
  bool isPICDefault() const override { return true; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return true; }

  llvm::codegenoptions::DebugInfoFormat getDefaultDebugFormat() const override {
    return llvm::codegenoptions::DIF_CodeView;
  }
  llvm::DebuggerKind getDefaultDebuggerTuning() const override {
    return llvm::DebuggerKind::Default;
  }
  unsigned GetDefaultDwarfVersion() const override { return 4; }

  llvm::ExceptionHandling
  GetExceptionModel(const llvm::opt::ArgList &Args) const override {
    return llvm::ExceptionHandling::WinEH;
  }

  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return ToolChain::RLT_CompilerRT;
  }
  UnwindLibType GetUnwindLibType(const llvm::opt::ArgList &Args) const override;
  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return ToolChain::CST_Libcxx;
  }
  const char *getDefaultLinker() const override { return "lld-link"; }

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  /// Adds the start-up objects, unless -nostartfiles.
  virtual void addStartFiles(const llvm::opt::ArgList &Args,
                             llvm::opt::ArgStringList &CmdArgs,
                             bool IsDLL) const {}
  /// Adds the unwind library.
  virtual void addUnwindLibArgs(const llvm::opt::ArgList &Args,
                                llvm::opt::ArgStringList &CmdArgs) const;
  /// Adds the directories of the system libraries to the library search path.
  virtual void addSystemLibraryDirs(const llvm::opt::ArgList &Args,
                                    std::vector<std::string> &LibDirs) const {}
  /// Adds the C library and the system libraries, unless -nolibc.
  virtual void addSystemLibArgs(const llvm::opt::ArgList &Args,
                                llvm::opt::ArgStringList &CmdArgs) const = 0;
  /// Removes the default libraries that objects name but these targets never
  /// link.
  virtual void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                                   llvm::opt::ArgStringList &CmdArgs) const;

  /// Returns the Control Flow Guard mode: "cf" unless -mguard= selects
  /// "cf-nochecks" or "none".
  StringRef getGuardMode(const llvm::opt::ArgList &Args) const;

protected:
  Tool *buildLinker() const override;

  /// Adds the defaults that Clang takes from the driver arguments, and
  /// replaces the exception models these targets lack by SEH, with a warning.
  void translateCommonArgs(const llvm::opt::DerivedArgList &Args,
                           llvm::opt::DerivedArgList &DAL) const;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H
