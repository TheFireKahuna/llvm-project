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
#include "llvm/ADT/StringExtras.h"
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

void WindowsItaniumBaseToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind /*DeviceOffloadKind*/) const {
  // Avoid LTO link errors from available_externally dllimport inlines.
  if (!DriverArgs.hasFlag(options::OPT__SLASH_Zc_dllexportInlines,
                          options::OPT_fno_dllexport_inlines, false))
    CC1Args.push_back("-fno-dllexport-inlines");

  // An explicit default visibility marks the shared-library boundary in both
  // directions: a marked definition is exported and a marked declaration is
  // imported. A function that the translation unit does not define is called
  // through the import table, which the linker binds directly when the
  // definition is in the image. Unmarked data is local unless the user asks
  // for auto-import.
  if (!DriverArgs.hasArg(options::OPT_mdefault_visibility_export_mapping_EQ))
    CC1Args.push_back("-mdefault-visibility-export-mapping=explicit");
  DriverArgs.AddLastArg(CC1Args,
                        options::OPT_mdefault_visibility_export_mapping_EQ);
  if (!DriverArgs.hasArg(options::OPT_fauto_import,
                         options::OPT_fno_auto_import))
    CC1Args.push_back("-fno-auto-import");
  if (DriverArgs.hasFlag(options::OPT_fno_plt, options::OPT_fplt,
                         getArch() == llvm::Triple::x86_64))
    CC1Args.push_back("-fno-plt");

  // clang-cl translates /guard: together with its other options.
  if (!getDriver().IsCLMode()) {
    GuardOptions Guard = getGuardOptions(DriverArgs);
    if (Guard.Checks)
      CC1Args.push_back("-cfguard");
    else if (Guard.Tables)
      CC1Args.push_back("-cfguard-no-checks");
    if (Guard.EHCont && !isCETCompatible())
      CC1Args.push_back("-ehcontguard");
  }
  // Every object records its EH continuation targets, so that the table of
  // an image linked with Control Flow Guard is complete whichever objects it
  // combines. Only SEH handlers have targets; the others emit nothing.
  if (isCETCompatible())
    CC1Args.push_back("-ehcontguard");
}

WindowsItaniumBaseToolChain::GuardOptions
WindowsItaniumBaseToolChain::getGuardOptions(const ArgList &Args) const {
  GuardOptions Guard;
  for (const Arg *A :
       Args.filtered(options::OPT_mguard_EQ, options::OPT__SLASH_guard)) {
    A->claim();
    StringRef Value = A->getValue();
    if (Value.equals_insensitive("cf")) {
      Guard.Tables = Guard.Checks = true;
    } else if (Value.equals_insensitive("cf-nochecks") ||
               Value.equals_insensitive("cf,nochecks")) {
      Guard.Tables = true;
      Guard.Checks = false;
    } else if (Value.equals_insensitive("cf-")) {
      Guard.Tables = Guard.Checks = false;
    } else if (Value.equals_insensitive("ehcont")) {
      Guard.EHCont = true;
    } else if (Value.equals_insensitive("ehcont-")) {
      Guard.EHCont = false;
    } else if (Value.equals_insensitive("none")) {
      Guard = GuardOptions();
    } else {
      getDriver().Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << Value;
    }
  }
  return Guard;
}

void WindowsItaniumBaseToolChain::addGuardLinkArgs(const ArgList &Args,
                                                   ArgStringList &CmdArgs,
                                                   bool IsDLL) const {
  if (isCETCompatible())
    CmdArgs.push_back("-cetcompat");

  GuardOptions Guard = getGuardOptions(Args);
  if (isCETCompatible())
    Guard.EHCont |= Guard.Tables;
  if (!Guard.Tables && !Guard.EHCont)
    return;

  // lld-link honours the last -guard: argument only, so every mode goes into
  // one. Export suppression narrows an executable's valid indirect-call
  // targets to the exports whose addresses are taken.
  SmallVector<StringRef, 3> Modes;
  if (Guard.Tables)
    Modes.push_back("cf");
  if (Guard.EHCont)
    Modes.push_back("ehcont");
  if (Guard.Tables && !IsDLL)
    Modes.push_back("exportsuppress");
  CmdArgs.push_back(
      Args.MakeArgString("-guard:" + llvm::join(Modes, ",")));
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
  CmdArgs.push_back("-defaultlib:libc++.dll.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-defaultlib:libc++experimental.lib");
}

// ============================================================================
// Utilities
// ============================================================================

const char *WindowsItaniumBaseToolChain::GetLibraryArg(
    const ArgList &Args, const ArgStringList &CmdArgs, StringRef Name) const {
  // Exact filenames bypass the platform's prefix/suffix search.
  if (Name.consume_front(":"))
    return Args.MakeArgString(Name);
  if (Name.ends_with_insensitive(".lib") || Name.ends_with_insensitive(".a"))
    return Args.MakeArgString(Name);

  // lld-link does not implement -l lookup. Search each linker directory in
  // order, preferring an import library over a static archive. Unprefixed
  // libraries remain available for Windows SDK and other native libraries.
  const std::string Names[] = {("lib" + Name + ".dll.lib").str(),
                              ("lib" + Name + ".lib").str(),
                              (Name + ".lib").str()};
  for (const auto &File : Names) {
    if (getVFS().exists(File))
      return Args.MakeArgString(File);
  }
  for (StringRef Arg : CmdArgs) {
    if (!Arg.consume_front_insensitive("-libpath:") &&
        !Arg.consume_front_insensitive("/libpath:"))
      continue;
    for (const auto &File : Names) {
      SmallString<128> Path(Arg);
      llvm::sys::path::append(Path, File);
      if (getVFS().exists(Path))
        return Args.MakeArgString(Path);
    }
  }
  // Preserve lld-link's diagnostic and native library search on a miss.
  return Args.MakeArgString(Names[2]);
}

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
  // -rdynamic exports every default-visibility definition on ELF. An image
  // here exports the definitions its sources mark with default visibility
  // without it, and an unmarked one when compiled with
  // -mdefault-visibility-export-mapping=all, so the linker needs nothing.
  Args.ClaimAllArgs(options::OPT_rdynamic);

  for (auto It = CmdArgs.begin(); It != CmdArgs.end();) {
    StringRef Value(*It);
    // Resolve -l only after collecting every linker argument, including a
    // -Wl,/libpath: that follows the library on the command line.
    if (Value == "-l" && It + 1 != CmdArgs.end()) {
      *It = GetLibraryArg(Args, CmdArgs, *(It + 1));
      It = CmdArgs.erase(It + 1);
      continue;
    }
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
