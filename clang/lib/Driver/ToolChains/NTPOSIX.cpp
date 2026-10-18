//===--- NTPOSIX.cpp - NT-POSIX ToolChain ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NTPOSIX.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

NTPOSIXToolChain::NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                                   const ArgList &Args)
    : WindowsItaniumBaseToolChain(D, Triple, Args) {
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkdir))
    WinSdkDir = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkversion))
    WinSdkVersion = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsysroot))
    WinSysRoot = A->getValue();
  if (!D.SysRoot.empty()) {
    SmallString<128> LibPath(D.SysRoot);
    llvm::sys::path::append(LibPath, "lib", Triple.str());
    if (getVFS().exists(LibPath))
      getFilePaths().push_back(std::string(LibPath));
    LibPath = D.SysRoot;
    llvm::sys::path::append(LibPath, "lib");
    if (getVFS().exists(LibPath))
      getFilePaths().push_back(std::string(LibPath));
  }
}

DerivedArgList *NTPOSIXToolChain::TranslateArgs(const DerivedArgList &Args,
                                                BoundArch BA,
                                                Action::OffloadKind OFK) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  for (Arg *A : Args)
    DAL->append(A);
  translateCommonArgs(Args, *DAL);
  return DAL;
}

void NTPOSIXToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBaseToolChain::addClangTargetOptions(DriverArgs, CC1Args, BA,
                                                     DeviceOffloadKind);

  // The libc headers declare its functions and data imported from libc.dll,
  // unless the program links libc statically.
  if (!DriverArgs.hasArg(options::OPT_static))
    CC1Args.push_back("-D_LIBC_DLL");

  // The C library is always thread-capable.
  CC1Args.push_back("-pthread");

  // Every object lists its EH continuation targets, since nt.dll's unwinder
  // lands only on the targets an image's table lists.
  CC1Args.push_back("-ehcontguard");

  // wchar_t is 16 bits in the Win32 face, which keeps it apart from the POSIX
  // face's 32-bit one; alone, -fshort-wchar would not.
  if (const Arg *A = DriverArgs.getLastArg(options::OPT_fshort_wchar))
    getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
        << A->getAsString(DriverArgs) << getTriple().str();
  bool Win32Face = DriverArgs.hasArg(options::OPT_mwin32);
  if (Win32Face) {
    CC1Args.push_back("-mwin32");
    CC1Args.push_back("-fwchar-type=short");
    CC1Args.push_back("-fno-signed-wchar");
  }

  // Headers under a Win32 root are Win32 declaration regions. A unit that can
  // reach one, or is in the Win32 face, takes the Microsoft syntax extensions
  // the Windows headers use, such as __uuidof and __declspec(uuid).
  std::vector<std::string> Roots = getWin32HeaderRoots(DriverArgs);
  for (const std::string &Root : Roots)
    CC1Args.push_back(DriverArgs.MakeArgString("-fwin32-header-root=" + Root));
  for (const std::string &Dir : DriverArgs.getAllArgValues(options::OPT_iwin32)) {
    CC1Args.push_back("-idirafter");
    CC1Args.push_back(DriverArgs.MakeArgString(Dir));
  }
  if (Win32Face || !Roots.empty())
    CC1Args.push_back("-fms-extensions");
}

std::vector<std::string>
NTPOSIXToolChain::getWindowsSDKIncludeDirs(const ArgList &Args) const {
  std::vector<std::string> Dirs;
  std::string SDKDir, IncludeVersion, LibVersion;
  int Major = 0;
  if (Args.hasArg(options::OPT_nostdinc, options::OPT_nostdlibinc) ||
      !llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                              SDKDir, Major, IncludeVersion, LibVersion) ||
      Major < 10)
    return Dirs;
  if (!WinSdkDir && !WinSysRoot && WinSdkVersion)
    IncludeVersion = std::string(*WinSdkVersion);
  // The SDK's headers expect Visual C++'s vcruntime headers. Windows Itanium's
  // wrappers, ahead of the SDK, supply them and adapt the SDK to clang; behind
  // the C library, they never stand in for one of its headers.
  if (!Args.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Dir(getDriver().ResourceDir);
    llvm::sys::path::append(Dir, "include", "win32_itanium_wrappers");
    Dirs.push_back(std::string(Dir));
  }
  for (const char *Sub : {"shared", "um", "winrt", "cppwinrt"}) {
    SmallString<128> Dir(SDKDir);
    llvm::sys::path::append(Dir, "Include", IncludeVersion, Sub);
    if (getVFS().exists(Dir))
      Dirs.push_back(std::string(Dir));
  }
  return Dirs;
}

std::vector<std::string>
NTPOSIXToolChain::getWin32HeaderRoots(const ArgList &Args) const {
  std::vector<std::string> Roots = getWindowsSDKIncludeDirs(Args);
  for (const std::string &Dir : Args.getAllArgValues(options::OPT_iwin32))
    Roots.push_back(Dir);
  return Roots;
}

static void addLibCIncludeArgs(const ToolChain &TC, const ArgList &DriverArgs,
                               ArgStringList &CC1Args, StringRef Root) {
  SmallString<128> Path(Root);
  llvm::sys::path::append(Path, "include", TC.getTripleString());
  if (TC.getVFS().exists(Path))
    ToolChain::addSystemInclude(DriverArgs, CC1Args, Path);
  Path = Root;
  llvm::sys::path::append(Path, "include");
  if (TC.getVFS().exists(Path))
    ToolChain::addSystemInclude(DriverArgs, CC1Args, Path);
}

void NTPOSIXToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Path(getDriver().ResourceDir);
    llvm::sys::path::append(Path, "include");
    addSystemInclude(DriverArgs, CC1Args, Path);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // llvm-libc's headers, in the sysroot or beside the toolchain.
  if (!getDriver().SysRoot.empty())
    addLibCIncludeArgs(*this, DriverArgs, CC1Args, getDriver().SysRoot);
  SmallString<128> Root(getDriver().Dir);
  llvm::sys::path::append(Root, "..");
  addLibCIncludeArgs(*this, DriverArgs, CC1Args, Root);

  // The Windows SDK's headers, after the C library's, so that a POSIX header
  // is never found among them.
  for (const std::string &Dir : getWindowsSDKIncludeDirs(DriverArgs))
    addSystemInclude(DriverArgs, CC1Args, Dir);
}

void NTPOSIXToolChain::addRequiredFile(const ArgList &Args,
                                       ArgStringList &CmdArgs, const char *Name,
                                       bool DefaultLib) const {
  if (std::optional<std::string> Path = GetFilePathIfExists(Name))
    CmdArgs.push_back(
        Args.MakeArgString((DefaultLib ? "-defaultlib:" : "") + *Path));
  else
    getDriver().Diag(diag::err_drv_no_such_file) << Name;
}

void NTPOSIXToolChain::AddCXXStdlibLibArgs(const ArgList &Args,
                                           ArgStringList &CmdArgs) const {
  if (!Args.hasArg(options::OPT_static))
    return WindowsItaniumBaseToolChain::AddCXXStdlibLibArgs(Args, CmdArgs);
  addRequiredFile(Args, CmdArgs, "libc++.lib", /*DefaultLib=*/true);
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-defaultlib:libc++experimental.lib");
}

void NTPOSIXToolChain::addStartFiles(const ArgList &Args,
                                     ArgStringList &CmdArgs, bool IsDLL) const {
  // crt_tls.obj provides every image's TLS directory. crt_tls_cleanup.obj
  // registers libc's thread-detach callback, which libc.dll already runs, so
  // only an executable carries it; a second registration would run the
  // cleanup twice when a thread exits.
  if (IsDLL) {
    addRequiredFile(Args, CmdArgs, "dllcrt.obj", /*DefaultLib=*/false);
  } else {
    addRequiredFile(Args, CmdArgs, "crt1.obj", /*DefaultLib=*/false);
    addRequiredFile(Args, CmdArgs, "crt_do_start.obj", /*DefaultLib=*/false);
  }
  addRequiredFile(Args, CmdArgs, "crt_tls.obj", /*DefaultLib=*/false);
  if (!IsDLL)
    addRequiredFile(Args, CmdArgs, "crt_tls_cleanup.obj",
                    /*DefaultLib=*/false);
  for (const char *Name : {"crt_gs.obj", "crt_cfg.obj", "crt_loadcfg.obj"})
    addRequiredFile(Args, CmdArgs, Name, /*DefaultLib=*/false);
}

void NTPOSIXToolChain::addUnwindLibArgs(const ArgList &Args,
                                        ArgStringList &CmdArgs) const {
  if (!Args.hasArg(options::OPT_static))
    return WindowsItaniumBaseToolChain::addUnwindLibArgs(Args, CmdArgs);
  addRequiredFile(Args, CmdArgs, "libunwind.lib", /*DefaultLib=*/true);
}

void NTPOSIXToolChain::addSystemLibArgs(const ArgList &Args,
                                        ArgStringList &CmdArgs) const {
  // llvm-libc, as libc.dll's import library or with -static as an archive,
  // named as the other runtimes are, and the system import libraries it is
  // layered on.
  if (Args.hasArg(options::OPT_static))
    addRequiredFile(Args, CmdArgs, "libc.lib", /*DefaultLib=*/true);
  else
    CmdArgs.push_back("-defaultlib:libc.dll.lib");
  for (const char *Name : {"ntdll.lib", "sspicli.lib", "bcryptprimitives.lib"})
    addRequiredFile(Args, CmdArgs, Name, /*DefaultLib=*/true);
}

void NTPOSIXToolChain::addNoDefaultLibArgs(const ArgList &Args,
                                           ArgStringList &CmdArgs) const {
  WindowsItaniumBaseToolChain::addNoDefaultLibArgs(Args, CmdArgs);
  CmdArgs.push_back("-nodefaultlib:ucrt");
}
