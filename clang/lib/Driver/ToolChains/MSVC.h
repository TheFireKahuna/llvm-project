//===--- MSVC.h - MSVC ToolChain Implementations ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MSVC_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MSVC_H

#include "clang/Driver/Compilation.h"
#include "clang/Driver/CudaInstallationDetector.h"
#include "clang/Driver/LazyDetector.h"
#include "clang/Driver/RocmInstallationDetector.h"
#include "clang/Driver/SyclInstallationDetector.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/Frontend/Debug/Options.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

namespace clang {
namespace driver {
namespace tools {

/// Visual studio tools.
namespace visualstudio {
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("visualstudio::Linker", "linker", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // end namespace visualstudio

class LLVM_LIBRARY_VISIBILITY ARM64XObjcopy : public Tool {
public:
  ARM64XObjcopy(const ToolChain &TC)
      : Tool("ARM64XObjcopy", "llvm-objcopy", TC) {}

  bool hasIntegratedCPP() const override { return false; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // end namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY MSVCToolChain : public ToolChain {
public:
  MSVCToolChain(const Driver &D, const llvm::Triple &Triple,
                const llvm::opt::ArgList &Args);

  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, BoundArch BA,
                Action::OffloadKind DeviceOffloadKind) const override;

  UnwindTableLevel
  getDefaultUnwindTableLevel(const llvm::opt::ArgList &Args) const override;
  bool isPICDefault() const override;
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override;
  bool isPICDefaultForced() const override;

  /// Set CodeView as the default debug info format for non-MachO binary
  /// formats, and to DWARF otherwise. Users can use -gcodeview and -gdwarf to
  /// override the default.
  llvm::codegenoptions::DebugInfoFormat getDefaultDebugFormat() const override {
    return getTriple().isOSBinFormatCOFF() ? llvm::codegenoptions::DIF_CodeView
                                           : llvm::codegenoptions::DIF_DWARF;
  }

  /// Set the debugger tuning to "default", since we're definitely not tuning
  /// for GDB.
  llvm::DebuggerKind getDefaultDebuggerTuning() const override {
    return llvm::DebuggerKind::Default;
  }

  unsigned GetDefaultDwarfVersion() const override {
    return 4;
  }

  std::string getSubDirectoryPath(llvm::SubDirectoryType Type,
                                  llvm::StringRef SubdirParent = "") const;
  std::string getSubDirectoryPath(llvm::SubDirectoryType Type,
                                  llvm::Triple::ArchType TargetArch) const;

  bool getIsVS2017OrNewer() const {
    return VSLayout == llvm::ToolsetLayout::VS2017OrNewer;
  }

  void
  AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const override;
  llvm::StringRef
  GetCXXStdlibName(const llvm::opt::ArgList &DriverArgs) const override;
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  void AddCudaIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;

  void AddHIPIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                         llvm::opt::ArgStringList &CC1Args) const override;

  void addOffloadRTLibs(unsigned ActiveKinds, const llvm::opt::ArgList &Args,
                        llvm::opt::ArgStringList &CmdArgs) const override;

  void addSYCLIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                          llvm::opt::ArgStringList &CC1Args) const override;

  bool getWindowsSDKLibraryPath(
      const llvm::opt::ArgList &Args, std::string &path) const;
  bool getUniversalCRTLibraryPath(const llvm::opt::ArgList &Args,
                                  std::string &path) const;
  bool useUniversalCRT() const;
  VersionTuple
  computeMSVCVersion(const Driver *D,
                     const llvm::opt::ArgList &Args) const override;

  std::string ComputeEffectiveClangTriple(const llvm::opt::ArgList &Args,
                                          BoundArch BA,
                                          types::ID InputType) const override;
  SanitizerMask
  getSupportedSanitizers(BoundArch BA,
                         Action::OffloadKind DeviceOffloadKind) const override;

  void printVerboseInfo(raw_ostream &OS) const override;

  bool FoundMSVCInstall() const { return !VCToolChainPath.empty(); }

  void
  addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                        llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                        Action::OffloadKind DeviceOffloadKind) const override;

protected:
  void AddMSVCStdlibMultilibIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                        llvm::opt::ArgStringList &CC1Args,
                                        bool HonorNostdincxx) const;
  void AddMSVCStdlibIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                llvm::opt::ArgStringList &CC1Args) const;

  Tool *getTool(Action::ActionClass AC) const override;
  Tool *buildLinker() const override;
  Tool *buildAssembler() const override;

private:
  std::optional<llvm::StringRef> WinSdkDir, WinSdkVersion, WinSysRoot;
  std::string VCToolChainPath;
  llvm::ToolsetLayout VSLayout = llvm::ToolsetLayout::OlderVS;
  LazyDetector<CudaInstallationDetector> CudaInstallation;
  LazyDetector<RocmInstallationDetector> RocmInstallation;
  LazyDetector<SYCLInstallationDetector> SYCLInstallation;
  mutable std::unique_ptr<tools::ARM64XObjcopy> Objcopy;
};

// The Windows SDK and Universal CRT lookup, shared by the toolchains that use
// them. WinSdkDir, WinSdkVersion and WinSysRoot are the values of /winsdkdir,
// /winsdkversion and /winsysroot.

/// Gets the library path required to link against the Windows SDK.
bool getWindowsSDKLibraryPath(llvm::vfs::FileSystem &VFS,
                              std::optional<llvm::StringRef> WinSdkDir,
                              std::optional<llvm::StringRef> WinSdkVersion,
                              std::optional<llvm::StringRef> WinSysRoot,
                              llvm::Triple::ArchType Arch, std::string &Path);

/// Gets the library path required to link against the Universal CRT.
bool getUniversalCRTLibraryPath(llvm::vfs::FileSystem &VFS,
                                std::optional<llvm::StringRef> WinSdkDir,
                                std::optional<llvm::StringRef> WinSdkVersion,
                                std::optional<llvm::StringRef> WinSysRoot,
                                llvm::Triple::ArchType Arch, std::string &Path);

/// Adds the directories that the environment variable Var lists, separated by
/// semicolons, as system include directories. Returns whether it listed any.
bool addSystemIncludesFromEnv(const llvm::opt::ArgList &DriverArgs,
                              llvm::opt::ArgStringList &CC1Args,
                              llvm::StringRef Var);

/// Adds the Universal CRT's include directory.
void addUniversalCRTIncludeArgs(llvm::vfs::FileSystem &VFS,
                                std::optional<llvm::StringRef> WinSdkDir,
                                std::optional<llvm::StringRef> WinSdkVersion,
                                std::optional<llvm::StringRef> WinSysRoot,
                                const llvm::opt::ArgList &DriverArgs,
                                llvm::opt::ArgStringList &CC1Args);

/// Adds the Windows SDK's include directories.
void addWindowsSDKIncludeArgs(llvm::vfs::FileSystem &VFS,
                              std::optional<llvm::StringRef> WinSdkDir,
                              std::optional<llvm::StringRef> WinSdkVersion,
                              std::optional<llvm::StringRef> WinSysRoot,
                              const llvm::opt::ArgList &DriverArgs,
                              llvm::opt::ArgStringList &CC1Args);

/// Adds the HIP runtime from the ROCm installation, and the profile runtime
/// for HIP device code under -fprofile-generate, when ActiveKinds includes HIP.
void addHIPRuntimeLibArgs(
    const ToolChain &TC,
    const LazyDetector<RocmInstallationDetector> &RocmInstallation,
    unsigned ActiveKinds, const llvm::opt::ArgList &Args,
    llvm::opt::ArgStringList &CmdArgs);

/// Expands the clang-cl arguments whose meaning does not depend on the
/// toolchain: /O..., /permissive, /permissive- and -Dname#value.
llvm::opt::DerivedArgList *
translateMSVCCompatibleArgs(const ToolChain &TC,
                            const llvm::opt::DerivedArgList &Args,
                            Action::OffloadKind OFK);

void AddSystemIncludeWithSubfolder(const llvm::opt::ArgList &DriverArgs,
                                   llvm::opt::ArgStringList &CC1Args,
                                   const std::string &folder,
                                   const Twine &subfolder1,
                                   const Twine &subfolder2 = "",
                                   const Twine &subfolder3 = "");

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MSVC_H
