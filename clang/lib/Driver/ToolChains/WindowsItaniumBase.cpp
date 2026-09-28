//===--- WindowsItaniumBase.cpp - Windows Itanium base ToolChain ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItaniumBase.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

void tools::windowsitanium::Linker::ConstructJob(
    Compilation &C, const JobAction &JA, const InputInfo &Output,
    const InputInfoList &Inputs, const ArgList &Args,
    const char *LinkingOutput) const {
  const ToolChain &TC = getToolChain();
  ArgStringList CmdArgs;

  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString("-out:" + Twine(Output.getFilename())));

  CmdArgs.push_back(TC.getArch() == llvm::Triple::aarch64 ? "-machine:arm64"
                                                          : "-machine:x64");
  CmdArgs.push_back("-nologo");

  // Every library search path comes from the driver: a Visual Studio
  // developer shell's LIB names the Visual C++ runtime libraries, which these
  // targets never link.
  CmdArgs.push_back("-lldignoreenv");

  if (Args.hasArg(options::OPT_shared))
    CmdArgs.push_back("-dll");

  for (const auto &LibPath : Args.getAllArgValues(options::OPT_L))
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + LibPath));

  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l)) {
      StringRef Lib = A.getValue();
      CmdArgs.push_back(Lib.ends_with_insensitive(".lib")
                            ? Args.MakeArgString(Lib)
                            : Args.MakeArgString(Lib + ".lib"));
      continue;
    }
    A.renderAsInput(Args, CmdArgs);
  }

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(TC.GetProgramPath("lld-link")), CmdArgs, Inputs,
      Output));
}

WindowsItaniumBaseToolChain::WindowsItaniumBaseToolChain(
    const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : ToolChain(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);
}

Tool *WindowsItaniumBaseToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

void WindowsItaniumBaseToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  // A declaration or definition with default visibility is imported or
  // exported: default visibility marks the DLL boundary, as it marks the
  // shared-object boundary on ELF.
  if (const Arg *A = DriverArgs.getLastArg(
          options::OPT_mdefault_visibility_export_mapping_EQ))
    A->render(DriverArgs, CC1Args);
  else
    CC1Args.push_back("-mdefault-visibility-export-mapping=explicit");
}

void WindowsItaniumBaseToolChain::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdlibinc,
                        options::OPT_nostdincxx))
    return;

  const Driver &D = getDriver();
  SmallString<128> TargetPath(D.Dir);
  llvm::sys::path::append(TargetPath, "..", "include", getTripleString());
  llvm::sys::path::append(TargetPath, "c++", "v1");
  if (getVFS().exists(TargetPath))
    addSystemInclude(DriverArgs, CC1Args, TargetPath);

  SmallString<128> InstallPath(D.Dir);
  llvm::sys::path::append(InstallPath, "..", "include", "c++", "v1");
  if (getVFS().exists(InstallPath))
    addSystemInclude(DriverArgs, CC1Args, InstallPath);

  if (!D.SysRoot.empty()) {
    SmallString<128> SysRootPath(D.SysRoot);
    llvm::sys::path::append(SysRootPath, "include", "c++", "v1");
    if (getVFS().exists(SysRootPath))
      addSystemInclude(DriverArgs, CC1Args, SysRootPath);
  }
}

void WindowsItaniumBaseToolChain::translateCommonArgs(
    const DerivedArgList &Args, DerivedArgList &DAL) const {
  const OptTable &Opts = getDriver().getOpts();
  // __declspec, which the system headers use, and __cxa_atexit, which the
  // Itanium C++ ABI's C++ runtime provides.
  if (!Args.hasArgNoClaim(options::OPT_fdeclspec, options::OPT_fno_declspec))
    DAL.AddFlagArg(nullptr, Opts.getOption(options::OPT_fdeclspec));
  if (!Args.hasArgNoClaim(options::OPT_fuse_cxa_atexit,
                          options::OPT_fno_use_cxa_atexit))
    DAL.AddFlagArg(nullptr, Opts.getOption(options::OPT_fuse_cxa_atexit));

  for (Arg *A : Args.filtered(options::OPT_fdwarf_exceptions,
                              options::OPT_fwasm_exceptions)) {
    getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
        << A->getAsString(Args) << getTriple().str();
    DAL.eraseArg(A->getOption().getID());
    DAL.AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
  }
}
