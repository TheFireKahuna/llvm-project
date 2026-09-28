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
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

WindowsItaniumToolChain::WindowsItaniumToolChain(const Driver &D,
                                                 const llvm::Triple &Triple,
                                                 const ArgList &Args)
    : WindowsItaniumBaseToolChain(D, Triple, Args) {
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
  // clang_rt.ucrt_memory.lib imports them. clang_rt.aligned_alloc, built
  // beside wincrt, provides aligned_alloc and posix_memalign, which the
  // Universal CRT lacks. oldnames.lib maps the POSIX names to the Universal
  // CRT's underscored ones, as it does for MSVC.
  CmdArgs.push_back(Args.MakeArgString("-defaultlib:" +
                                       getCompilerRTBasename(Args, "wincrt")));
  CmdArgs.push_back(Args.MakeArgString(
      "-defaultlib:" + getCompilerRTBasename(Args, "ucrt_memory")));
  CmdArgs.push_back(Args.MakeArgString(
      "-defaultlib:" + getCompilerRTBasename(Args, "aligned_alloc")));
  for (const char *Lib :
       {"ucrt.lib", "kernel32.lib", "ntdll.lib", "oldnames.lib", "user32.lib",
        "advapi32.lib", "shell32.lib"})
    CmdArgs.push_back(Args.MakeArgString("-defaultlib:" + Twine(Lib)));

  // wincrt's executable start-up checks that the process heap is the segment
  // heap, which only the manifest can request, so an executable embeds the
  // manifest shipped beside wincrt.
  if (Args.hasArg(options::OPT_shared, options::OPT__SLASH_LD,
                  options::OPT__SLASH_LDd))
    return;
  SmallString<128> Manifest(
      llvm::sys::path::parent_path(getCompilerRT(Args, "wincrt")));
  llvm::sys::path::append(Manifest, "segment_heap.manifest");
  if (getVFS().exists(Manifest)) {
    CmdArgs.push_back("-manifest:embed");
    CmdArgs.push_back(Args.MakeArgString("-manifestinput:" + Manifest));
  }
}

void WindowsItaniumToolChain::addNoDefaultLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  WindowsItaniumBaseToolChain::addNoDefaultLibArgs(Args, CmdArgs);
  // Objects compiled with _CRT_STDIO_ISO_WIDE_SPECIFIERS name this library of
  // Visual C++, whose one symbol wincrt defines.
  CmdArgs.push_back("-nodefaultlib:iso_stdio_wide_specifiers");
}
