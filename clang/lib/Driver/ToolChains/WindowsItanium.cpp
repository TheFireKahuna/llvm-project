//===--- WindowsItanium.cpp - Windows Itanium ToolChain -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItanium.h"
#include "MSVC.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Driver/Driver.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

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
WindowsItaniumToolChain::TranslateArgs(const DerivedArgList &Args,
                                       StringRef BoundArch,
                                       Action::OffloadKind OFK) const {
  DerivedArgList *DAL = translateMSVCCompatibleArgs(*this, Args, OFK);
  // The stack protector is on by default; /GS- has to reach the option that
  // turns it off, which clang-cl's own handling of /GS never emits.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_GS, options::OPT__SLASH_GS_))
    if (A->getOption().matches(options::OPT__SLASH_GS_))
      DAL->AddFlagArg(
          A, getDriver().getOpts().getOption(options::OPT_fno_stack_protector));
  // clang-cl's /std: names a language standard with MSVC's spellings. Only
  // the spelling is translated; without /std: the target keeps clang's own
  // default standard rather than MSVC's.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_std)) {
    StringRef Std = llvm::StringSwitch<StringRef>(A->getValue())
                        .Case("c11", "c11")
                        .Case("c17", "c17")
                        .Case("clatest", "c23")
                        .Case("c++14", "c++14")
                        .Case("c++17", "c++17")
                        .Case("c++20", "c++20")
                        .Case("c++23preview", "c++23")
                        .Case("c++latest", "c++26")
                        .Default("");
    if (!Std.empty()) {
      DAL->eraseArg(options::OPT__SLASH_std);
      DAL->AddJoinedArg(A, getDriver().getOpts().getOption(options::OPT_std_EQ),
                        Std);
    }
  }
  // Only the SEH and SjLj exception models exist on this target.
  for (Arg *A : Args.filtered(options::OPT_fdwarf_exceptions,
                              options::OPT_fwasm_exceptions)) {
    getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
        << A->getAsString(Args) << getTriple().str();
    DAL->eraseArg(A->getOption().getID());
    DAL->AddFlagArg(
        A, getDriver().getOpts().getOption(options::OPT_fseh_exceptions));
  }
  return DAL;
}

VersionTuple
WindowsItaniumToolChain::computeMSVCVersion(const Driver *D,
                                            const ArgList &Args) const {
  VersionTuple MSVT = ToolChain::computeMSVCVersion(D, Args);
  // The UCRT and Windows SDK headers need the Microsoft keyword extensions,
  // which this toolchain enables below. Without a compatibility version clang
  // treats -fms-extensions as targeting a pre-2015 MSVC and keeps that
  // compiler's quirks: narrowing in braced initialization only warns, so it
  // no longer causes substitution failure, and unions accept reference
  // members. Name the same version the MSVC toolchain defaults to so only the
  // keyword extensions remain; the identity macros stay off because this is
  // not a known MSVC environment.
  if (MSVT.empty() && !Args.hasArg(options::OPT_fno_ms_extensions))
    MSVT = VersionTuple(19, 33);
  return MSVT;
}

void WindowsItaniumToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBaseToolChain::addClangTargetOptions(DriverArgs, CC1Args,
                                                     DeviceOffloadKind);

  if (!DriverArgs.hasArg(options::OPT_fno_ms_extensions))
    CC1Args.push_back("-fms-extensions");

  // As with MinGW, the C runtime provides the underscore-prefixed POSIX
  // names (_access, _open, _vsnprintf, ...).
  CC1Args.push_back("-D__MSVCRT__");

  // The UCRT is always linked dynamically. _DLL makes its headers declare its
  // functions and data imported, as they are under MSVC's /MD.
  CC1Args.push_back("-D_DLL");

  // %s and %ls in the wide printf and scanf functions take char and wchar_t
  // strings, as ISO C specifies.
  CC1Args.push_back("-D_CRT_STDIO_ISO_WIDE_SPECIFIERS");

  // The standard C functions are not deprecated.
  CC1Args.push_back("-D_CRT_SECURE_NO_WARNINGS");

  // The UCRT's inline functions have external linkage, so a C++ module can
  // export declarations that use them.
  CC1Args.push_back("-D_STATIC_INLINE_UCRT_FUNCTIONS=0");

  // The UCRT has no clock_gettime.
  CC1Args.push_back("-UCLOCK_REALTIME");

  // Linker options. -mthreads is MinGW's; the UCRT is always thread-safe.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole,
                   options::OPT_mthreads, options::OPT_marm64x})
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();
}

void WindowsItaniumToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include");
    // The UCRT and the Windows SDK assume the Visual C compiler and its
    // vcruntime. This target's wrappers over their headers, and its
    // replacements for the vcruntime ones, sit ahead of them and chain
    // through #include_next. They are C-library headers, so -nostdlibinc
    // hides them with the UCRT.
    if (!DriverArgs.hasArg(options::OPT_nostdlibinc))
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args,
                                    getDriver().ResourceDir, "include",
                                    "win32_itanium_wrappers");
  }

  for (const auto &Path : DriverArgs.getAllArgValues(options::OPT__SLASH_imsvc))
    addSystemInclude(DriverArgs, CC1Args, Path);

  // Unlike the MSVC toolchain, the implicit INCLUDE and EXTERNAL_INCLUDE of a
  // Visual Studio developer shell are ignored, since they name the Visual C
  // headers; only an explicit /external:env: variable is read.
  for (const auto &Var :
       DriverArgs.getAllArgValues(options::OPT__SLASH_external_env)) {
    if (std::optional<std::string> Val = llvm::sys::Process::GetEnv(Var)) {
      SmallVector<StringRef, 8> Dirs;
      StringRef(*Val).split(Dirs, ";", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
      addSystemIncludes(DriverArgs, CC1Args, Dirs);
    }
  }

  // -nostdlibinc hides the UCRT headers but not the Windows SDK ones.
  if (!DriverArgs.hasArg(options::OPT_nostdlibinc)) {
    std::string UniversalCRTSdkPath;
    std::string UCRTVersion;
    if (llvm::getUniversalCRTSdkDir(getVFS(), WinSdkDir, WinSdkVersion,
                                    WinSysRoot, UniversalCRTSdkPath,
                                    UCRTVersion)) {
      if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
          WinSdkVersion.has_value())
        UCRTVersion = *WinSdkVersion;
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, UniversalCRTSdkPath,
                                    "Include", UCRTVersion, "ucrt");
    }
  }

  // As MSVCToolChain::AddClangSystemIncludeArgs does, but never with the
  // Visual C headers: this target's own vcruntime.h, vadefs.h and intrin.h
  // replace them.
  std::string WindowsSDKDir;
  int major = 0;
  std::string windowsSDKIncludeVersion;
  std::string windowsSDKLibVersion;
  if (llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                             WindowsSDKDir, major, windowsSDKIncludeVersion,
                             windowsSDKLibVersion)) {
    if (major >= 10)
      if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
          WinSdkVersion.has_value())
        windowsSDKIncludeVersion = windowsSDKLibVersion = *WinSdkVersion;
    if (major >= 8) {
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", windowsSDKIncludeVersion,
                                    "shared");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", windowsSDKIncludeVersion, "um");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", windowsSDKIncludeVersion,
                                    "winrt");
      if (major >= 10) {
        llvm::VersionTuple Tuple;
        if (!Tuple.tryParse(windowsSDKIncludeVersion) &&
            Tuple.getSubminor().value_or(0) >= 17134)
          AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                        "Include", windowsSDKIncludeVersion,
                                        "cppwinrt");
      }
    } else {
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include");
    }
  }
}

bool WindowsItaniumToolChain::getWindowsSDKLibraryPath(
    const ArgList &Args, std::string &path) const {
  std::string sdkPath;
  int sdkMajor = 0;
  std::string windowsSDKIncludeVersion;
  std::string windowsSDKLibVersion;

  path.clear();
  if (!llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                              sdkPath, sdkMajor, windowsSDKIncludeVersion,
                              windowsSDKLibVersion))
    return false;

  llvm::SmallString<128> libPath(sdkPath);
  llvm::sys::path::append(libPath, "Lib");
  if (sdkMajor >= 10)
    if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
        WinSdkVersion.has_value())
      windowsSDKLibVersion = *WinSdkVersion;
  if (sdkMajor >= 8)
    llvm::sys::path::append(libPath, windowsSDKLibVersion, "um");
  return llvm::appendArchToWindowsSDKLibPath(sdkMajor, libPath, getArch(),
                                             path);
}

bool WindowsItaniumToolChain::getUniversalCRTLibraryPath(
    const ArgList &Args, std::string &Path) const {
  std::string UniversalCRTSdkPath;
  std::string UCRTVersion;

  Path.clear();
  if (!llvm::getUniversalCRTSdkDir(getVFS(), WinSdkDir, WinSdkVersion,
                                   WinSysRoot, UniversalCRTSdkPath,
                                   UCRTVersion))
    return false;

  if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
      WinSdkVersion.has_value())
    UCRTVersion = *WinSdkVersion;

  StringRef ArchName = llvm::archToWindowsSDKArch(getArch());
  if (ArchName.empty())
    return false;

  llvm::SmallString<128> LibPath(UniversalCRTSdkPath);
  llvm::sys::path::append(LibPath, "Lib", UCRTVersion, "ucrt", ArchName);

  Path = std::string(LibPath);
  return true;
}

StringRef
WindowsItaniumToolChain::getExecutableEntryPoint(const ArgList &Args) const {
  const Arg *A = Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
  if (A && A->getOption().matches(options::OPT_mwindows))
    return "WinMainCRTStartup";
  return "mainCRTStartup";
}

void WindowsItaniumToolChain::addSystemLinkArgs(const ArgList &Args,
                                                ArgStringList &CmdArgs,
                                                bool IsDLL) const {
  // lld checks the manifest against conflicting options and input resources,
  // so this applies to executables with their own start-up as well.
  if (!IsDLL)
    CmdArgs.push_back("-manifest:embed,heap=segment");

  // A delay-loaded import is called through a table the loader writes, which
  // no indirect-call check covers. Giving that table a section of its own
  // lets the loader keep it read-only except while it resolves an import.
  CmdArgs.push_back("-delayload-protect");

  // The library search path is entirely driver-owned: a Visual Studio
  // developer shell's LIB and VC installation variables must not reach
  // lld-link, or MSVC's runtime libraries would be found.
  CmdArgs.push_back("-lldignoreenv");

  if (Args.hasArg(options::OPT_nostdlib))
    return;
  std::string Path;
  if (getUniversalCRTLibraryPath(Args, Path))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + Path));
  if (getWindowsSDKLibraryPath(Args, Path))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + Path));
}

bool WindowsItaniumToolChain::addLibCArgs(const ArgList &Args,
                                          ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-defaultlib:ucrt.lib");
  CmdArgs.push_back("-defaultlib:kernel32.lib");

  // wincrt provides the start-up code, __cxa_atexit and the security cookie,
  // and calls ntdll directly.
  if (getVFS().exists(getCompilerRT(Args, "wincrt")))
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-defaultlib:") + getCompilerRTBasename(Args, "wincrt")));
  CmdArgs.push_back("-defaultlib:ntdll.lib");

  // ucrtbase.dll exports memcpy, memset, memmove, memcmp, memchr and the SEH
  // personality, but ucrt.lib does not import them: Microsoft supplies them
  // through vcruntime.lib, which this target never links. This import
  // library, beside wincrt, provides them.
  if (getVFS().exists(getCompilerRT(Args, "ucrt_memory")))
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-defaultlib:") + getCompilerRTBasename(Args, "ucrt_memory")));

  CmdArgs.push_back("-defaultlib:user32");
  CmdArgs.push_back("-defaultlib:advapi32");
  CmdArgs.push_back("-defaultlib:shell32");
  return true;
}

void WindowsItaniumToolChain::addNoDefaultLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  // Objects compiled with _CRT_STDIO_ISO_WIDE_SPECIFIERS request
  // iso_stdio_wide_specifiers.lib, which lives with the Visual C libraries;
  // wincrt defines the symbol it provides.
  for (const char *Lib :
       {"msvcrt", "msvcrtd", "vcruntime", "vcruntimed", "libcmt", "libcmtd",
        "oldnames", "ucrtd", "iso_stdio_wide_specifiers"})
    CmdArgs.push_back(Args.MakeArgString(Twine("-nodefaultlib:") + Lib));
}
