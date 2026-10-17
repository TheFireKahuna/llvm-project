//===--- WindowsItanium.cpp - Windows Itanium ToolChain -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItanium.h"
#include "MSVC.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

WindowsItaniumToolChain::WindowsItaniumToolChain(const Driver &D,
                                                 const llvm::Triple &Triple,
                                                 const ArgList &Args)
    : WindowsItaniumBaseToolChain(D, Triple, Args),
      CudaInstallation(D, Triple, Args), RocmInstallation(D, Triple, Args),
      SYCLInstallation(D, Triple, Args) {
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkdir))
    WinSdkDir = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkversion))
    WinSdkVersion = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsysroot))
    WinSysRoot = A->getValue();
}

DerivedArgList *
WindowsItaniumToolChain::TranslateArgs(const DerivedArgList &Args, BoundArch BA,
                                       Action::OffloadKind OFK) const {
  DerivedArgList *DAL = translateMSVCCompatibleArgs(*this, Args, OFK);
  translateCommonArgs(Args, *DAL);
  // The Windows SDK headers need the Microsoft extensions.
  if (!Args.hasArgNoClaim(options::OPT_fms_extensions,
                          options::OPT_fno_ms_extensions))
    DAL->AddFlagArg(
        nullptr, getDriver().getOpts().getOption(options::OPT_fms_extensions));
  return DAL;
}

VersionTuple
WindowsItaniumToolChain::computeMSVCVersion(const Driver *D,
                                            const ArgList &Args) const {
  VersionTuple MSVT = ToolChain::computeMSVCVersion(D, Args);
  // Without a compatibility version, -fms-extensions emulates a Visual C++
  // older than 2015, whose quirks break standard C++: narrowing in a braced
  // initializer only warns, and unions accept reference members. The version
  // the MSVC toolchain defaults to leaves only the keyword extensions the
  // Windows SDK needs; no _MSC_VER is defined, since the environment is not
  // MSVC.
  if (MSVT.empty() && Args.hasFlag(options::OPT_fms_extensions,
                                   options::OPT_fno_ms_extensions, true))
    MSVT = VersionTuple(19, 33);
  return MSVT;
}

void WindowsItaniumToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBaseToolChain::addClangTargetOptions(DriverArgs, CC1Args, BA,
                                                     DeviceOffloadKind);

  // The Universal CRT is always linked dynamically, so its headers declare
  // its functions and data imported, as under /MD.
  CC1Args.push_back("-D_DLL");
}

void WindowsItaniumToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include");
    // The Universal CRT and Windows SDK headers expect Visual C++ and its
    // vcruntime headers. The wrappers, ahead of them, adapt them to clang and
    // replace the vcruntime headers, so -nostdlibinc hides them too.
    if (!DriverArgs.hasArg(options::OPT_nostdlibinc))
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args,
                                    getDriver().ResourceDir, "include",
                                    "win32_itanium_wrappers");
  }

  for (const auto &Path : DriverArgs.getAllArgValues(options::OPT__SLASH_imsvc))
    addSystemInclude(DriverArgs, CC1Args, Path);
  for (const auto &Var :
       DriverArgs.getAllArgValues(options::OPT__SLASH_external_env))
    addSystemIncludesFromEnv(DriverArgs, CC1Args, Var);

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  addUniversalCRTIncludeArgs(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                             DriverArgs, CC1Args);
  addWindowsSDKIncludeArgs(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                           DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::addSystemLibraryDirs(
    const ArgList &Args, std::vector<std::string> &LibDirs) const {
  std::string Path;
  if (getUniversalCRTLibraryPath(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                                 getArch(), Path))
    LibDirs.push_back(Path);
  if (getWindowsSDKLibraryPath(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                               getArch(), Path))
    LibDirs.push_back(Path);
}

void WindowsItaniumToolChain::addSystemLibArgs(const ArgList &Args,
                                               ArgStringList &CmdArgs) const {
  // wincrt provides the start-up code and bridges the Universal CRT to the
  // Itanium C++ ABI. ucrtbase.dll exports memcpy, memset and the other
  // functions that Microsoft ships in vcruntime.lib rather than ucrt.lib, and
  // clang_rt.ucrt_memory.lib imports them. oldnames.lib maps the POSIX names
  // to the Universal CRT's underscored ones, as it does for MSVC.
  // CRT's underscored ones, as it does for MSVC. Every image registers its
  // termination functions with the process's registries, and ends the process
  // through exit, which clang_rt.wincrt_dynamic.dll holds; with -static they
  // are in the image.
  CmdArgs.push_back(Args.MakeArgString("-defaultlib:" +
                                       getCompilerRTBasename(Args, "wincrt")));
  CmdArgs.push_back(Args.MakeArgString(
      "-defaultlib:" +
      getCompilerRTBasename(Args, Args.hasArg(options::OPT_static)
                                      ? "wincrt_static"
                                      : "wincrt_dynamic")));
  CmdArgs.push_back(Args.MakeArgString(
      "-defaultlib:" + getCompilerRTBasename(Args, "ucrt_memory")));
  //
  // The API-set umbrella comes last, so that kernel32.lib still binds what it
  // covers, while registry, security, COM and shell-core functions bind to the
  // API sets that kernelbase, sechost, combase and shcore host, rather than to
  // advapi32, ole32 and shell32, which load msvcrt.dll or connect to win32k.
  // user32, gdi32 and shell32 stay out, so that a program connects to win32k
  // only by naming them. A GUI program, selected with -mwindows, gets user32,
  // which every window needs; GDI drawing stays a library it names.
  for (const char *Lib : {"ucrt.lib", "kernel32.lib", "ntdll.lib",
                          "oldnames.lib", "onecore_apiset.lib"})
    CmdArgs.push_back(Args.MakeArgString("-defaultlib:" + Twine(Lib)));
  if (const Arg *A =
          Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
      A && A->getOption().matches(options::OPT_mwindows))
    CmdArgs.push_back("-defaultlib:user32.lib");
}

void WindowsItaniumToolChain::addNoDefaultLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  WindowsItaniumBaseToolChain::addNoDefaultLibArgs(Args, CmdArgs);
  // Objects compiled with _CRT_STDIO_ISO_WIDE_SPECIFIERS name this library of
  // Visual C++, whose one symbol wincrt defines.
  CmdArgs.push_back("-nodefaultlib:iso_stdio_wide_specifiers");
}

void WindowsItaniumToolChain::AddCudaIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  CudaInstallation->AddCudaIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::AddHIPIncludeArgs(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args) const {
  RocmInstallation->AddHIPIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::addSYCLIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  SYCLInstallation->addSYCLIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::addOffloadRTLibs(unsigned ActiveKinds,
                                               const ArgList &Args,
                                               ArgStringList &CmdArgs) const {
  addHIPRuntimeLibArgs(*this, RocmInstallation, ActiveKinds, Args, CmdArgs);
}

void WindowsItaniumToolChain::printVerboseInfo(raw_ostream &OS) const {
  CudaInstallation->print(OS);
  RocmInstallation->print(OS);
}
