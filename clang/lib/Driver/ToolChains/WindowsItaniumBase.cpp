//===--- WindowsItaniumBase.cpp - Shared base for Win+Itanium -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItaniumBase.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

// ============================================================================
// Constructor
// ============================================================================

WindowsItaniumBaseToolChain::WindowsItaniumBaseToolChain(
    const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : ToolChain(D, Triple, Args), CudaInstallation(D, Triple, Args),
      RocmInstallation(D, Triple, Args), SYCLInstallation(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);

  SmallString<128> LibPath(D.Dir);
  llvm::sys::path::append(LibPath, "..", "lib");
  if (getVFS().exists(LibPath))
    getFilePaths().push_back(std::string(LibPath));

  SmallString<128> TargetLibPath(D.Dir);
  llvm::sys::path::append(TargetLibPath, "..", "lib", Triple.str());
  if (getVFS().exists(TargetLibPath))
    getFilePaths().push_back(std::string(TargetLibPath));
}

// ============================================================================
// Platform defaults
// ============================================================================

ToolChain::UnwindTableLevel
WindowsItaniumBaseToolChain::getDefaultUnwindTableLevel(
    const ArgList &Args) const {
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::arm ||
      getArch() == llvm::Triple::thumb || getArch() == llvm::Triple::aarch64)
    return UnwindTableLevel::Asynchronous;
  return UnwindTableLevel::None;
}

bool WindowsItaniumBaseToolChain::isPICDefault() const {
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

bool WindowsItaniumBaseToolChain::isPIEDefault(const ArgList &Args) const {
  return false;
}

bool WindowsItaniumBaseToolChain::isPICDefaultForced() const {
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

SanitizerMask WindowsItaniumBaseToolChain::getSupportedSanitizers() const {
  SanitizerMask Res = ToolChain::getSupportedSanitizers();
  Res |= SanitizerKind::Address;
  Res |= SanitizerKind::PointerCompare;
  Res |= SanitizerKind::PointerSubtract;
  Res |= SanitizerKind::Fuzzer;
  Res |= SanitizerKind::FuzzerNoLink;
  Res &= ~SanitizerKind::CFIMFCall;
  return Res;
}

llvm::ExceptionHandling
WindowsItaniumBaseToolChain::GetExceptionModel(const ArgList &Args) const {
  if (Args.hasArg(options::OPT_fsjlj_exceptions))
    return llvm::ExceptionHandling::SjLj;
  // SEH with Itanium personality on 64-bit; SJLJ on 32-bit.
  // Table-based SEH (DISPATCHER_CONTEXT) only exists on x64/ARM64.
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::aarch64)
    return llvm::ExceptionHandling::WinEH;
  return llvm::ExceptionHandling::SjLj;
}

// ============================================================================
// C++ standard library
// ============================================================================

ToolChain::CXXStdlibType
WindowsItaniumBaseToolChain::GetCXXStdlibType(const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++") {
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

void WindowsItaniumBaseToolChain::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  DriverArgs.getLastArg(options::OPT_stdlib_EQ);

  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdincxx,
                        options::OPT_nostdlibinc))
    return;

  const Driver &D = getDriver();

  // Target-specific path for multi-target installations.
  SmallString<128> TargetPath(D.Dir);
  llvm::sys::path::append(TargetPath, "..", "include", getTripleString());
  llvm::sys::path::append(TargetPath, "c++", "v1");
  if (D.getVFS().exists(TargetPath))
    addSystemInclude(DriverArgs, CC1Args, TargetPath);

  SmallString<128> InstallPath(D.Dir);
  llvm::sys::path::append(InstallPath, "..", "include", "c++", "v1");
  if (D.getVFS().exists(InstallPath))
    addSystemInclude(DriverArgs, CC1Args, InstallPath);

  for (const std::string &LibPath : getFilePaths()) {
    SmallString<128> LibIncludePath(LibPath);
    llvm::sys::path::append(LibIncludePath, "..", "include", "c++", "v1");
    if (D.getVFS().exists(LibIncludePath)) {
      addSystemInclude(DriverArgs, CC1Args, LibIncludePath);
      break;
    }
  }

  if (!D.SysRoot.empty()) {
    SmallString<128> SysrootPath(D.SysRoot);
    llvm::sys::path::append(SysrootPath, "include", "c++", "v1");
    if (D.getVFS().exists(SysrootPath))
      addSystemInclude(DriverArgs, CC1Args, SysrootPath);
  }
}

void WindowsItaniumBaseToolChain::AddCXXStdlibLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  CmdArgs.push_back("c++.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("c++experimental.lib");
}

// ============================================================================
// Utilities
// ============================================================================

void WindowsItaniumBaseToolChain::AddRuntimeLibSearchPaths(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  auto AddLibPath = [&](const std::string &LibPath) {
    if (getVFS().exists(LibPath))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));
  };

  for (const std::string &LibPath : getFilePaths())
    AddLibPath(LibPath);

  for (const std::string &LibPath : getLibraryPaths())
    AddLibPath(LibPath);

  std::string CRTPath = getCompilerRTPath();
  if (getVFS().exists(CRTPath))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + CRTPath));
}

void WindowsItaniumBaseToolChain::NormalizeLLDLinkArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  // -rdynamic: export all symbols from the executable, equivalent to
  // -export-dynamic on ELF. lld-link uses /export-all-symbols.
  if (Args.hasArg(options::OPT_rdynamic))
    CmdArgs.push_back("-export-all-symbols");

  for (auto It = CmdArgs.begin(); It != CmdArgs.end();) {
    StringRef Value(*It);
    // PE image-version flags are currently unstable with the custom
    // Windows-Itanium/NTPOSIX lld-link path and can crash the linker during
    // try-link probes. Drop them until lld grows reliable support here.
    if (Value.starts_with_insensitive("/version:") ||
        Value.starts_with_insensitive("-version:")) {
      It = CmdArgs.erase(It);
      continue;
    }
    // -rpath is an ELF concept with no PE/COFF equivalent. Windows DLL
    // search uses the exe directory, system dirs, and PATH instead.
    // Strip silently so POSIX-oriented build systems don't produce warnings.
    // Handles both "-rpath=VALUE" and "-rpath VALUE" (two separate args).
    if (Value.starts_with_insensitive("-rpath")) {
      bool is_separate = (Value == "-rpath") && (It + 1) != CmdArgs.end();
      It = CmdArgs.erase(It);
      if (is_separate && It != CmdArgs.end())
        It = CmdArgs.erase(It); // consume the path argument
      continue;
    }
    ++It;
  }
}

void WindowsItaniumBaseToolChain::AddSystemIncludeWithSubfolder(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    const std::string &Folder, const Twine &Sub1, const Twine &Sub2,
    const Twine &Sub3) const {
  llvm::SmallString<128> P(Folder);
  llvm::sys::path::append(P, Sub1, Sub2, Sub3);
  addSystemInclude(DriverArgs, CC1Args, P);
}

// ============================================================================
// Offload / GPU forwarding
// ============================================================================

void WindowsItaniumBaseToolChain::AddCudaIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  CudaInstallation->AddCudaIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::AddHIPIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  RocmInstallation->AddHIPIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::addSYCLIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  SYCLInstallation->addSYCLIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::addOffloadRTLibs(
    unsigned ActiveKinds, const ArgList &Args, ArgStringList &CmdArgs) const {
  if (Args.hasArg(options::OPT_no_hip_rt) || Args.hasArg(options::OPT_r))
    return;
  if (ActiveKinds & Action::OFK_HIP) {
    CmdArgs.append({Args.MakeArgString(StringRef("-libpath:") +
                                       RocmInstallation->getLibPath()),
                    "amdhip64.lib"});
  }
}

void WindowsItaniumBaseToolChain::printVerboseInfo(raw_ostream &OS) const {
  CudaInstallation->print(OS);
  RocmInstallation->print(OS);
}
