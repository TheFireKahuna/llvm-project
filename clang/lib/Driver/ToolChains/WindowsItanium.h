//===--- WindowsItanium.h - Windows Itanium ToolChain -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows Itanium toolchain using lld-link with Itanium C++ ABI.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H

#include "clang/Driver/CudaInstallationDetector.h"
#include "clang/Driver/LazyDetector.h"
#include "clang/Driver/RocmInstallationDetector.h"
#include "clang/Driver/SyclInstallationDetector.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Frontend/Debug/Options.h"
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

class LLVM_LIBRARY_VISIBILITY WindowsItaniumToolChain : public ToolChain {
public:
  enum class LibCMode { System, LLVMLibC };

  WindowsItaniumToolChain(const Driver &D, const llvm::Triple &Triple,
                          const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
                Action::OffloadKind DeviceOffloadKind) const override;

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

  CXXStdlibType GetCXXStdlibType(const llvm::opt::ArgList &Args) const override;

  RuntimeLibType GetDefaultRuntimeLibType() const override;
  LibCMode GetDefaultLibCMode() const;
  bool usesLLVMLibC() const { return GetDefaultLibCMode() == LibCMode::LLVMLibC; }

  UnwindLibType GetDefaultUnwindLibType() const override {
    return ToolChain::UNW_CompilerRT;
  }

  const char *getDefaultLinker() const override { return "lld-link"; }

  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                 llvm::opt::ArgStringList &CC1Args) const override;
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;
  void AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                           llvm::opt::ArgStringList &CmdArgs) const override;

  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args,
                             Action::OffloadKind DeviceOffloadKind) const override;

  void AddCudaIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;
  void AddHIPIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                         llvm::opt::ArgStringList &CC1Args) const override;
  void addSYCLIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;
  void addOffloadRTLibs(unsigned ActiveKinds, const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;

  void printVerboseInfo(raw_ostream &OS) const override;

  bool useUniversalCRT() const;
  
  /// Get Windows SDK library path for linking.
  /// Returns true if a valid SDK library path was found.
  bool getWindowsSDKLibraryPath(const llvm::opt::ArgList &Args,
                                std::string &Path) const;

  /// Get Universal CRT library path for linking.
  /// Returns true if a valid UCRT library path was found.
  bool getUniversalCRTLibraryPath(const llvm::opt::ArgList &Args,
                                  std::string &Path) const;

  /// Returns true if Windows SDK was found via explicit flags or
  /// auto-detection. When false, the toolchain falls back to INCLUDE/LIB
  /// environment variables.
  bool FoundWindowsSDK() const { return !WindowsSDKDir.empty(); }

protected:
  Tool *buildLinker() const override;

  /// Add a system include path with optional subfolders, similar to MSVC.
  void AddSystemIncludeWithSubfolder(const llvm::opt::ArgList &DriverArgs,
                                     llvm::opt::ArgStringList &CC1Args,
                                     const std::string &Folder,
                                     const Twine &Subfolder1,
                                     const Twine &Subfolder2 = "",
                                     const Twine &Subfolder3 = "") const;

private:
  LazyDetector<CudaInstallationDetector> CudaInstallation;
  LazyDetector<RocmInstallationDetector> RocmInstallation;
  LazyDetector<SYCLInstallationDetector> SYCLInstallation;

  /// VC toolchain path for MSVC runtime headers.
  std::string VCToolChainPath;
  llvm::ToolsetLayout VSLayout = llvm::ToolsetLayout::OlderVS;

  /// Windows SDK configuration from command line arguments.
  std::optional<llvm::StringRef> WinSdkDir;
  std::optional<llvm::StringRef> WinSdkVersion;
  std::optional<llvm::StringRef> WinSysRoot;

  /// Cached Windows SDK path. Empty if not found.
  std::string WindowsSDKDir;
  int WindowsSDKMajor = 0;
  std::string WindowsSDKIncludeVersion;
  std::string WindowsSDKLibVersion;
};

} // namespace toolchains
} // namespace driver
} // namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_WINDOWSITANIUM_H
