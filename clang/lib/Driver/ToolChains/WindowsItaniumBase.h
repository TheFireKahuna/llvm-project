//===--- WindowsItaniumBase.h - Shared base for Win+Itanium ---*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The base of the Windows toolchains that use the Itanium C++ ABI, lld-link,
/// compiler-rt, libunwind and libc++: Windows Itanium, with the UCRT, and
/// NT-POSIX, with llvm-libc.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUMBASE_H

#include "clang/Driver/CudaInstallationDetector.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/LazyDetector.h"
#include "clang/Driver/RocmInstallationDetector.h"
#include "clang/Driver/SyclInstallationDetector.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Frontend/Debug/Options.h"

namespace clang {
namespace driver {
namespace tools {
namespace windowsitanium {

/// Links with lld-link. The toolchain supplies the C runtime.
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

  // MSVC builds with /GS unless told otherwise, and so does clang-cl on this
  // target; the GNU-style driver matches, so that a translation unit gets the
  // same cookie whichever driver compiles it.
  LangOptions::StackProtectorMode
  GetDefaultStackProtectorLevel(bool KernelOrKext) const override {
    return LangOptions::SSPStrong;
  }

  llvm::ExceptionHandling
  GetExceptionModel(const llvm::opt::ArgList &Args) const override;

  SanitizerMask getSupportedSanitizers() const override;

  CXXStdlibType GetDefaultCXXStdlibType() const override {
    return ToolChain::CST_Libcxx;
  }

  CXXStdlibType GetCXXStdlibType(const llvm::opt::ArgList &Args) const override;

  RuntimeLibType GetDefaultRuntimeLibType() const override {
    return ToolChain::RLT_CompilerRT;
  }

  UnwindLibType GetDefaultUnwindLibType() const override {
    return ToolChain::UNW_CompilerRT;
  }

  const char *getDefaultLinker() const override { return "lld-link"; }

  /// The import model defaults and the guard modes; the derived toolchains
  /// call this before adding their own options.
  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args,
                        Action::OffloadKind DeviceOffloadKind) const override;

  /// Whether images are marked compatible with the hardware shadow stack.
  /// The EH continuation table then accompanies Control Flow Guard, so that
  /// every continuation the loader has to validate is listed.
  virtual bool isCETCompatible() const { return false; }

  /// Adds -cetcompat and the single -guard: argument that lld-link honours.
  void addGuardLinkArgs(const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs, bool IsDLL) const;

  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  // The C runtime's part of a link, which the linker adds in this order
  // around the arguments that do not depend on the C runtime. The functions
  // that return bool diagnose a missing file and return false.

  /// The entry point of an executable.
  virtual StringRef
  getExecutableEntryPoint(const llvm::opt::ArgList &Args) const {
    return "mainCRTStartup";
  }
  /// Arguments ahead of the -L library paths.
  virtual void addSystemLinkArgs(const llvm::opt::ArgList &Args,
                                 llvm::opt::ArgStringList &CmdArgs,
                                 bool IsDLL) const {}
  /// The start-up objects, unless -nostartfiles.
  virtual bool addStartFiles(const llvm::opt::ArgList &Args,
                             llvm::opt::ArgStringList &CmdArgs,
                             bool IsDLL) const {
    return true;
  }
  /// The C library, unless -nolibc.
  virtual bool addLibCArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const = 0;
  /// The default libraries that objects must not pull in.
  virtual void addNoDefaultLibArgs(const llvm::opt::ArgList &Args,
                                   llvm::opt::ArgStringList &CmdArgs) const = 0;
  /// Libraries that follow the inputs.
  virtual bool addPostInputLibs(const llvm::opt::ArgList &Args,
                                const InputInfoList &Inputs,
                                llvm::opt::ArgStringList &CmdArgs) const {
    return true;
  }

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
  Tool *buildLinker() const override;

  const char *GetLibraryArg(const llvm::opt::ArgList &Args,
                            const llvm::opt::ArgStringList &CmdArgs,
                            StringRef Name) const;

  struct GuardOptions {
    bool Tables = false; ///< address-taken function tables (cf, cf-nochecks)
    bool Checks = false; ///< instrumented indirect calls (cf)
    bool EHCont = false; ///< EH continuation table (ehcont)
  };
  /// The guard modes selected by -mguard= and /guard:, in command-line order.
  GuardOptions getGuardOptions(const llvm::opt::ArgList &Args) const;

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
