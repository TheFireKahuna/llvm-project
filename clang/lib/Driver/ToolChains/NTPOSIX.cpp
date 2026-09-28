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

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

NTPOSIXToolChain::NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                                   const ArgList &Args)
    : WindowsItaniumBaseToolChain(D, Triple, Args) {
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

  // NT's user-mode exception and APC dispatchers build their frames right
  // below the stack pointer, over the System V red zone, so a signal or an
  // exception that resumes would corrupt the locals of the interrupted leaf
  // function.
  if (getArch() == llvm::Triple::x86_64 &&
      !DriverArgs.hasArg(options::OPT_mred_zone, options::OPT_mno_red_zone))
    CC1Args.push_back("-disable-red-zone");

  // A 16-bit wchar_t would bind to the 32-bit one's C and C++ interfaces.
  if (const Arg *A = DriverArgs.getLastArg(options::OPT_fshort_wchar))
    getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
        << A->getAsString(DriverArgs) << getTriple().str();
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
