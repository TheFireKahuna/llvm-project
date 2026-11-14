//===--- WindowsItanium.cpp - Windows Itanium ToolChains ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItanium.h"
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

// lld-link looks a library up by the name it is given. The runtimes of these
// targets carry the "lib" prefix, as on ELF, and the Windows SDK's libraries
// do not, so -lname is libname.dll.lib, then libname.lib, where a library
// directory has one, and otherwise name.lib. -static takes only the archive
// and -l:file names the file, as for GNU ld.
static const char *getLibraryArg(const ToolChain &TC, const ArgList &Args,
                                 ArrayRef<std::string> LibDirs,
                                 StringRef Name) {
  if (Name.consume_front(":") || Name.ends_with_insensitive(".lib"))
    return Args.MakeArgString(Name);
  static const char *const Suffixes[] = {".dll.lib", ".lib"};
  for (const std::string &Dir : LibDirs) {
    for (const char *Suffix :
         ArrayRef(Suffixes).drop_front(Args.hasArg(options::OPT_static))) {
      SmallString<128> Path(Dir);
      llvm::sys::path::append(Path, "lib" + Name + Suffix);
      if (TC.getVFS().exists(Path))
        return Args.MakeArgString(Path);
    }
  }
  return Args.MakeArgString(Name + ".lib");
}

void tools::windowsitanium::Linker::ConstructJob(
    Compilation &C, const JobAction &JA, const InputInfo &Output,
    const InputInfoList &Inputs, const ArgList &Args,
    const char *LinkingOutput) const {
  const auto &TC =
      static_cast<const toolchains::WindowsItaniumBase &>(getToolChain());
  const Driver &D = TC.getDriver();
  ArgStringList CmdArgs;

  // lld-link is the only linker of these targets.
  if (const Arg *A = Args.getLastArg(options::OPT_fuse_ld_EQ);
      A && !StringRef(A->getValue()).equals_insensitive("lld") &&
      !StringRef(A->getValue()).equals_insensitive("lld-link"))
    D.Diag(diag::err_drv_unsupported_opt_for_target)
        << A->getAsString(Args) << TC.getTripleString();

  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(Twine("-out:") + Output.getFilename()));

  // Every library directory comes from the driver: the LIB of a Visual Studio
  // developer shell names the Visual C++ runtime libraries, which these
  // targets never link.
  CmdArgs.push_back("-lldignoreenv");

  if (Args.hasArg(options::OPT_shared, options::OPT__SLASH_LD,
                  options::OPT__SLASH_LDd)) {
    CmdArgs.push_back("-dll");
    // name.dll.lib, so that name.dll's import library and the static archive
    // name.lib can share a directory.
    SmallString<128> ImpLib(Output.getFilename());
    llvm::sys::path::replace_extension(ImpLib, "dll.lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImpLib));
  } else if (const Arg *A = Args.getLastArg(options::OPT_mwindows,
                                            options::OPT_mconsole)) {
    CmdArgs.push_back(A->getOption().matches(options::OPT_mwindows)
                          ? "-subsystem:windows"
                          : "-subsystem:console");
  }

  if (Args.hasArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    CmdArgs.push_back("-debug");

  // The image's timestamps are a hash of its contents, unless SOURCE_DATE_EPOCH
  // names them; a user's -timestamp:, which follows, replaces either.
  if (!std::getenv("SOURCE_DATE_EPOCH"))
    CmdArgs.push_back("-Brepro");

  // An executable exports what its sources mark, as a DLL does, so -rdynamic
  // has nothing to add.
  Args.ClaimAllArgs(options::OPT_rdynamic);

  std::vector<std::string> LibDirs = Args.getAllArgValues(options::OPT_L);
  llvm::append_range(LibDirs, TC.getFilePaths());
  for (const std::string &Dir : TC.getLibraryPaths())
    if (TC.getVFS().exists(Dir))
      LibDirs.push_back(Dir);
  if (std::string Dir = TC.getCompilerRTPath(); TC.getVFS().exists(Dir))
    LibDirs.push_back(Dir);
  for (const std::string &Dir : LibDirs)
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + Dir));
  // As GNU ld applies every -L to every -l, -l also searches the directories
  // passed to lld-link itself, after the driver's own.
  for (const Arg *A :
       Args.filtered(options::OPT_Wl_COMMA, options::OPT_Xlinker))
    for (StringRef Value : A->getValues())
      if (Value.consume_front_insensitive("-libpath:") ||
          Value.consume_front_insensitive("/libpath:"))
        LibDirs.push_back(Value.str());

  for (const InputInfo &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l))
      CmdArgs.push_back(getLibraryArg(TC, Args, LibDirs, A.getValue()));
    else
      // -Wl, and -Xlinker values go to lld-link as they are: it takes its own
      // options and ELF's spellings of those it shares with ld.lld.
      A.renderAsInput(Args, CmdArgs);
  }

  // The runtime libraries are default libraries, which lld-link searches after
  // every input, so that a definition among the inputs comes first and a
  // user's -nodefaultlib: removes one.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    // clang-cl has no C-only mode, so it links libc++ whenever it links.
    if (TC.ShouldLinkCXXStdlib(Args) ||
        (D.IsCLMode() && !Args.hasArg(options::OPT_nostdlibxx)))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);
    if (TC.GetUnwindLibType(Args) == ToolChain::UNW_CompilerRT)
      CmdArgs.push_back(Args.hasArg(options::OPT_static)
                            ? "-defaultlib:libunwind.lib"
                            : "-defaultlib:libunwind.dll.lib");
    CmdArgs.push_back(Args.MakeArgString(
        "-defaultlib:" + TC.getCompilerRTBasename(Args, "builtins")));
    if (!Args.hasArg(options::OPT_nolibc))
      TC.addSystemLibArgs(Args, CmdArgs);
    // Objects that clang-cl or MSVC compiled name the Visual C++ runtime
    // libraries, none of which these targets link.
    for (const char *Lib :
         {"-nodefaultlib:msvcrt", "-nodefaultlib:msvcrtd",
          "-nodefaultlib:vcruntime", "-nodefaultlib:vcruntimed",
          "-nodefaultlib:libcmt", "-nodefaultlib:libcmtd",
          "-nodefaultlib:ucrtd", "-nodefaultlib:iso_stdio_wide_specifiers"})
      CmdArgs.push_back(Lib);
  }

  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);
  TC.addOffloadRTLibs(C.getActiveOffloadKinds(), Args, CmdArgs);
  TC.addProfileRTLibs(Args, CmdArgs);

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(TC.GetProgramPath("lld-link")), CmdArgs, Inputs,
      Output));
}

WindowsItaniumBase::WindowsItaniumBase(const Driver &D,
                                       const llvm::Triple &Triple,
                                       const ArgList &Args)
    : WindowsSDKToolChain(D, Triple, Args) {
  getProgramPaths().push_back(D.Dir);

  // The runtimes installed beside the toolchain, outside a per-target
  // directory.
  SmallString<128> LibDir(D.Dir);
  llvm::sys::path::append(LibDir, "..", "lib");
  if (getVFS().exists(LibDir))
    getFilePaths().push_back(std::string(LibDir));
}

Tool *WindowsItaniumBase::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

DerivedArgList *
WindowsItaniumBase::TranslateArgs(const DerivedArgList &Args, BoundArch BA,
                                  Action::OffloadKind DeviceOffloadKind) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  const OptTable &Opts = getDriver().getOpts();

  // The toolchain's defaults come first, so that the user's options override
  // them: __declspec, which the system headers use, and __cxa_atexit, which
  // the C++ runtime provides.
  DAL->AddFlagArg(nullptr, Opts.getOption(options::OPT_fdeclspec));
  DAL->AddFlagArg(nullptr, Opts.getOption(options::OPT_fuse_cxa_atexit));
  // The Windows SDK's headers need the Microsoft extensions. Without a
  // compatibility version those emulate a Visual C++ older than 2015, whose
  // quirks break standard C++, so they come with the MSVC toolchain's default.
  if (getTriple().isWindowsItaniumEnvironment()) {
    DAL->AddFlagArg(nullptr, Opts.getOption(options::OPT_fms_extensions));
    if (Args.hasFlagNoClaim(options::OPT_fms_extensions,
                            options::OPT_fno_ms_extensions, true) &&
        !Args.hasArgNoClaim(options::OPT_fmsc_version,
                            options::OPT_fms_compatibility_version))
      DAL->AddJoinedArg(nullptr,
                        Opts.getOption(options::OPT_fms_compatibility_version),
                        "19.33");
  }

  for (Arg *A : Args) {
    // SEH is the only exception model.
    if (A->getOption().matches(options::OPT_fsjlj_exceptions) ||
        A->getOption().matches(options::OPT_fdwarf_exceptions) ||
        A->getOption().matches(options::OPT_fwasm_exceptions)) {
      getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
          << A->getAsString(Args) << getTripleString();
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
    } else {
      DAL->append(A);
    }
  }
  return DAL;
}

void WindowsItaniumBase::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  // The link uses these.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole})
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();
}

void WindowsItaniumBase::addClangWarningOptions(ArgStringList &CC1Args) const {
  // Only the cast gives a GetProcAddress result its type.
  CC1Args.push_back("-Wget-proc-address-type");
}

void WindowsItaniumBase::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdlibinc,
                        options::OPT_nostdincxx))
    return;

  // libc++'s headers beside the toolchain, the target's own first, then the
  // sysroot's.
  const Driver &D = getDriver();
  SmallString<128> Target(getStdlibIncludePath().value_or("")), Common(D.Dir),
      SysRoot(D.SysRoot);
  llvm::sys::path::append(Common, "..", "include");
  if (!SysRoot.empty())
    llvm::sys::path::append(SysRoot, "include");
  for (SmallString<128> *Dir : {&Target, &Common, &SysRoot}) {
    if (Dir->empty())
      continue;
    llvm::sys::path::append(*Dir, "c++", "v1");
    if (getVFS().exists(*Dir))
      addSystemInclude(DriverArgs, CC1Args, *Dir);
  }
}

void WindowsItaniumBase::AddCXXStdlibLibArgs(const ArgList &Args,
                                             ArgStringList &CmdArgs) const {
  CmdArgs.push_back(Args.hasArg(options::OPT_static)
                        ? "-defaultlib:libc++.lib"
                        : "-defaultlib:libc++.dll.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-defaultlib:libc++experimental.lib");
}

WindowsItaniumToolChain::WindowsItaniumToolChain(const Driver &D,
                                                 const llvm::Triple &Triple,
                                                 const ArgList &Args)
    : WindowsItaniumBase(D, Triple, Args) {
  std::string Path;
  if (getUniversalCRTLibraryPath(Args, Path))
    getFilePaths().push_back(Path);
  if (getWindowsSDKLibraryPath(Args, Path))
    getFilePaths().push_back(Path);
}

ToolChain::UnwindLibType
WindowsItaniumToolChain::GetUnwindLibType(const ArgList &Args) const {
  // libunwind is the platform's unwind library.
  const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ);
  if (!A || StringRef(A->getValue()) == "platform")
    return ToolChain::UNW_CompilerRT;
  return ToolChain::GetUnwindLibType(Args);
}

void WindowsItaniumToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBase::addClangTargetOptions(DriverArgs, CC1Args, BA,
                                            DeviceOffloadKind);
  // The Universal CRT is always linked dynamically, so its headers declare
  // its functions and data imported, as under /MD.
  CC1Args.push_back("-D_DLL");
}

void WindowsItaniumToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  bool StdlibInc = !DriverArgs.hasArg(options::OPT_nostdlibinc);
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include");
    // The Universal CRT's and the Windows SDK's headers expect Visual C++ and
    // its vcruntime headers. The wrappers adapt them to clang and stand in
    // for vcruntime's, so they come first and go with them.
    if (StdlibInc)
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args,
                                    getDriver().ResourceDir, "include",
                                    "win32_itanium_wrappers");
  }

  for (const auto &Path : DriverArgs.getAllArgValues(options::OPT__SLASH_imsvc))
    addSystemInclude(DriverArgs, CC1Args, Path);
  for (const auto &Var :
       DriverArgs.getAllArgValues(options::OPT__SLASH_external_env))
    addSystemIncludesFromEnv(DriverArgs, CC1Args, Var);

  if (StdlibInc) {
    addUniversalCRTIncludeArgs(DriverArgs, CC1Args);
    addWindowsSDKIncludeArgs(DriverArgs, CC1Args);
  }
}

void WindowsItaniumToolChain::addSystemLibArgs(const ArgList &Args,
                                               ArgStringList &CmdArgs) const {
  // wincrt starts the image and bridges the Universal CRT to the Itanium C++
  // ABI. clang_rt.ucrt_memory imports the functions ucrtbase exports and
  // Microsoft declares in vcruntime.
  for (const char *Lib : {"wincrt", "ucrt_memory"})
    CmdArgs.push_back(
        Args.MakeArgString("-defaultlib:" + getCompilerRTBasename(Args, Lib)));
  // The API-set umbrella comes last: kernel32 still binds what it covers, and
  // the registry, security, COM and shell-core functions bind to their API
  // sets, not to advapi32, ole32 and shell32, which load msvcrt.dll or
  // connect to win32k. A program connects to win32k only by naming user32,
  // gdi32 or shell32; -mwindows names user32, which every window needs.
  for (const char *Lib : {"-defaultlib:ucrt.lib", "-defaultlib:kernel32.lib",
                          "-defaultlib:ntdll.lib",
                          "-defaultlib:onecore_apiset.lib"})
    CmdArgs.push_back(Lib);
  if (const Arg *A =
          Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
      A && A->getOption().matches(options::OPT_mwindows))
    CmdArgs.push_back("-defaultlib:user32.lib");
}
