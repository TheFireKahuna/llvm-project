//===--- NTPOSIX.cpp - NT-POSIX ToolChain ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NTPOSIX.h"
#include "clang/Basic/DiagnosticDriver.h"
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
    SmallString<128> SysrootLib(D.SysRoot);
    llvm::sys::path::append(SysrootLib, "lib");
    if (getVFS().exists(SysrootLib))
      getFilePaths().push_back(std::string(SysrootLib));

    llvm::sys::path::append(SysrootLib, Triple.str());
    if (getVFS().exists(SysrootLib))
      getFilePaths().push_back(std::string(SysrootLib));
  }
}

void NTPOSIXToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBaseToolChain::addClangTargetOptions(DriverArgs, CC1Args,
                                                     DeviceOffloadKind);

  // The libc headers declare its functions and data imported from c.dll
  // unless it is linked statically.
  if (!DriverArgs.hasArg(options::OPT_static_libc))
    CC1Args.push_back("-D_LIBC_DLL");

  // wchar_t holds a code point, as on other POSIX systems.
  if (!DriverArgs.hasArg(options::OPT_fshort_wchar,
                         options::OPT_fno_short_wchar)) {
    CC1Args.push_back("-fwchar-type=int");
    CC1Args.push_back("-fsigned-wchar");
  }

  if (!DriverArgs.hasArg(options::OPT_fno_threadsafe_statics))
    CC1Args.push_back("-pthread");

  // The kernel's user-mode dispatchers (KiUserExceptionDispatcher and
  // KiUserApcDispatcher) build their frames directly below the stack
  // pointer, over the System V red zone, so a vectored exception handler
  // that continues execution or a signal delivered by APC would corrupt the
  // locals of an interrupted leaf function.
  if (!DriverArgs.hasArg(options::OPT_mred_zone, options::OPT_mno_red_zone))
    CC1Args.push_back("-disable-red-zone");

  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole})
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();
}

void NTPOSIXToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc))
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include");

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // llvm-libc provides every system header, in the sysroot or beside the
  // toolchain's libraries.
  auto AddLibCIncludes = [&](StringRef Root) {
    SmallString<128> Include(Root);
    llvm::sys::path::append(Include, "include", getTripleString());
    if (getVFS().exists(Include))
      addSystemInclude(DriverArgs, CC1Args, Include);

    Include = Root;
    llvm::sys::path::append(Include, "include");
    if (getVFS().exists(Include))
      addSystemInclude(DriverArgs, CC1Args, Include);
  };

  if (!getDriver().SysRoot.empty())
    AddLibCIncludes(getDriver().SysRoot);

  SmallString<128> Root(getDriver().Dir);
  llvm::sys::path::append(Root, "..");
  AddLibCIncludes(Root);

  for (const std::string &LibPath : getFilePaths()) {
    Root = LibPath;
    llvm::sys::path::append(Root, "..");
    AddLibCIncludes(Root);
  }
}

bool NTPOSIXToolChain::addRequiredFile(const ArgList &Args,
                                       ArgStringList &CmdArgs, const char *Name,
                                       bool DefaultLib) const {
  std::string Path = GetFilePath(Name);
  if (!getVFS().exists(Path)) {
    getDriver().Diag(diag::err_drv_no_such_file) << Name;
    return false;
  }
  CmdArgs.push_back(
      Args.MakeArgString(Twine(DefaultLib ? "-defaultlib:" : "") + Path));
  return true;
}

bool NTPOSIXToolChain::addStartFiles(const ArgList &Args,
                                     ArgStringList &CmdArgs, bool IsDLL) const {
  // crt_tls.obj provides the TLS directory of every image. crt_tls_cleanup.obj
  // registers the libc thread-detach callback, which c.dll already runs, so
  // only the executable carries it; a second registration would run the
  // cleanup twice when a thread exits.
  return addRequiredFile(Args, CmdArgs, IsDLL ? "dllcrt.obj" : "crt1.obj") &&
         (IsDLL || addRequiredFile(Args, CmdArgs, "crt_do_start.obj")) &&
         addRequiredFile(Args, CmdArgs, "crt_tls.obj") &&
         (IsDLL || addRequiredFile(Args, CmdArgs, "crt_tls_cleanup.obj")) &&
         addRequiredFile(Args, CmdArgs, "crt_gs.obj") &&
         addRequiredFile(Args, CmdArgs, "crt_cfg.obj") &&
         addRequiredFile(Args, CmdArgs, "crt_loadcfg.obj");
}

bool NTPOSIXToolChain::addLibCArgs(const ArgList &Args,
                                   ArgStringList &CmdArgs) const {
  // -static-libc links the archive, matching the declarations that the
  // absence of -D_LIBC_DLL selects, and keeps c.dll's import library out.
  if (!Args.hasArg(options::OPT_static_libc))
    return addRequiredFile(Args, CmdArgs, "c.lib", /*DefaultLib=*/true);
  if (!addRequiredFile(Args, CmdArgs, "libc.lib", /*DefaultLib=*/true))
    return false;
  CmdArgs.push_back("-nodefaultlib:c");
  CmdArgs.push_back("-nodefaultlib:c.lib");
  return true;
}

void NTPOSIXToolChain::addNoDefaultLibArgs(const ArgList &Args,
                                           ArgStringList &CmdArgs) const {
  for (const char *Lib : {"msvcrt", "msvcrtd", "vcruntime", "vcruntimed",
                          "ucrt", "ucrtd", "libcmt", "libcmtd", "oldnames"})
    CmdArgs.push_back(Args.MakeArgString(Twine("-nodefaultlib:") + Lib));
}

bool NTPOSIXToolChain::addPostInputLibs(const ArgList &Args,
                                        const InputInfoList &Inputs,
                                        ArgStringList &CmdArgs) const {
  // llvm-libc is layered on these system libraries. They follow the inputs so
  // that an explicit -lc under -nodefaultlibs, as a configure check links,
  // resolves as any other library does.
  bool LinkLibC = !Args.hasArg(options::OPT_nostdlib,
                               options::OPT_nodefaultlibs, options::OPT_nolibc);
  if (!LinkLibC)
    LinkLibC = llvm::any_of(Inputs, [](const InputInfo &Input) {
      if (Input.isFilename())
        return llvm::sys::path::filename(Input.getFilename())
            .equals_insensitive("c.lib");
      const Arg &A = Input.getInputArg();
      return A.getOption().matches(options::OPT_l) &&
             (StringRef(A.getValue()).equals_insensitive("c") ||
              StringRef(A.getValue()).equals_insensitive("c.lib"));
    });
  return !LinkLibC ||
         (addRequiredFile(Args, CmdArgs, "ntdll.lib", /*DefaultLib=*/true) &&
          addRequiredFile(Args, CmdArgs, "sspicli.lib", /*DefaultLib=*/true) &&
          addRequiredFile(Args, CmdArgs, "bcryptprimitives.lib",
                          /*DefaultLib=*/true));
}
