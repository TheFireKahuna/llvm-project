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
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

// Whether objects list their EH continuation targets and an image has their
// table, which the kernel checks a continuation against: NT-POSIX's unwinder
// continues only through it, and on Windows Itanium the x86-64 shadow stack
// brings the check.
static bool hasEHContGuard(const llvm::Triple &Triple) {
  return Triple.isWindowsNTPOSIXEnvironment() ||
         Triple.getArch() == llvm::Triple::x86_64;
}

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

  bool IsDLL = Args.hasArg(options::OPT_shared, options::OPT__SLASH_LD,
                           options::OPT__SLASH_LDd);
  if (IsDLL) {
    CmdArgs.push_back("-dll");
    // name.dll.lib, so that name.dll's import library and the static archive
    // name.lib can share a directory.
    SmallString<128> ImpLib(Output.getFilename());
    llvm::sys::path::replace_extension(ImpLib, "dll.lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImpLib));
  } else {
    if (const Arg *A =
            Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole))
      CmdArgs.push_back(A->getOption().matches(options::OPT_mwindows)
                            ? "-subsystem:windows"
                            : "-subsystem:console");
    // Every executable embeds a manifest, so that neither a loose manifest
    // nor a .local redirection changes what the loader binds. On Windows
    // Itanium it is the one shipped beside wincrt, which selects the segment
    // heap that wincrt's start-up requires.
    CmdArgs.push_back("-manifest:embed");
    if (TC.getTriple().isWindowsItaniumEnvironment()) {
      SmallString<128> Manifest(
          llvm::sys::path::parent_path(TC.getCompilerRT(Args, "wincrt")));
      llvm::sys::path::append(Manifest, "segment_heap.manifest");
      CmdArgs.push_back(Args.MakeArgString("-manifestinput:" + Manifest));
    }
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

  // The image runs with the hardware shadow stack.
  if (TC.getArch() == llvm::Triple::x86_64)
    CmdArgs.push_back("-cetcompat");

  // Control Flow Guard, under which an executable's exports are call targets
  // only once GetProcAddress returns them, and the table of EH continuation
  // targets. lld-link honors only the last -guard:, so one names them all and
  // a -guard: of the user's, which follows, replaces it.
  bool CFGuard = TC.getGuardMode(Args) != "none";
  std::string Guard = CFGuard ? "cf" : "";
  if (hasEHContGuard(TC.getTriple()))
    Guard += CFGuard ? ",ehcont" : "ehcont,nocf";
  if (CFGuard && !IsDLL)
    Guard += ",exportsuppress";
  if (!Guard.empty())
    CmdArgs.push_back(Args.MakeArgString("-guard:" + Guard));

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
    // The loader refuses an image without a load configuration, which the
    // builtins define and, on Windows Itanium, wincrt names.
    if (TC.getTriple().isWindowsNTPOSIXEnvironment())
      CmdArgs.push_back("-include:_load_config_used");
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
  // Unwind tables, with -ffreestanding too: the unwinder takes a function
  // without one for a leaf.
  DAL->AddFlagArg(nullptr,
                  Opts.getOption(options::OPT_fasynchronous_unwind_tables));
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
    switch (A->getOption().getID()) {
    // SEH is the only exception model.
    case options::OPT_fsjlj_exceptions:
    case options::OPT_fdwarf_exceptions:
    case options::OPT_fwasm_exceptions:
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
      [[fallthrough]];
    // Separate sections, from which the linker takes a function's or a data
    // object's extent, and unwind tables are not optional.
    case options::OPT_fno_function_sections:
    case options::OPT_fno_data_sections:
    case options::OPT_fno_asynchronous_unwind_tables:
    case options::OPT_fno_unwind_tables:
      getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
          << A->getAsString(Args) << getTripleString();
      A->claim();
      break;
    // clang-cl's /GS- only withholds the stack protector clang-cl asks for,
    // which would leave the toolchain's default.
    case options::OPT__SLASH_GS_:
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_fno_stack_protector));
      DAL->append(A);
      break;
    // clang-cl's Control Flow Guard modes are -mguard='s. Its EH continuation
    // values change nothing, since a table is complete only if every object
    // lists its targets, and any other value is left for clang-cl to reject.
    case options::OPT__SLASH_guard: {
      StringRef Value = A->getValue();
      const char *Mode =
          llvm::StringSwitch<const char *>(Value)
              .CaseLower("cf", Args.hasArg(options::OPT__SLASH_d2guardnochecks)
                                   ? "cf-nochecks"
                                   : "cf")
              .CaseLower("cf,nochecks", "cf-nochecks")
              .CaseLower("cf-", "none")
              .Default(nullptr);
      if (Mode)
        DAL->AddJoinedArg(A, Opts.getOption(options::OPT_mguard_EQ), Mode);
      else if (Value.equals_insensitive("ehcont") ||
               Value.equals_insensitive("ehcont-"))
        A->claim();
      else
        DAL->append(A);
      break;
    }
    default:
      DAL->append(A);
    }
  }
  return DAL;
}

StringRef WindowsItaniumBase::getGuardMode(const ArgList &Args) const {
  const Arg *A = Args.getLastArg(options::OPT_mguard_EQ);
  // ntposix.dll does not yet give an image its guard pointers.
  if (!A)
    return getTriple().isWindowsItaniumEnvironment() ? "cf" : "none";
  StringRef Mode = A->getValue();
  if (Mode == "cf" || Mode == "cf-nochecks" || Mode == "none")
    return Mode;
  getDriver().Diag(diag::err_drv_unsupported_option_argument)
      << A->getSpelling() << Mode;
  return "none";
}

void WindowsItaniumBase::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  // The link uses these.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole})
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();

  // Every object has Control Flow Guard's tables, and its checks unless
  // -mguard= leaves them out.
  CC1Args.push_back(getGuardMode(DriverArgs) == "cf" ? "-cfguard"
                                                     : "-cfguard-no-checks");
  if (hasEHContGuard(getTriple()))
    CC1Args.push_back("-ehcontguard");

  // A variable with vague linkage is unique, as GCC makes it on ELF.
  if (DriverArgs.hasFlag(options::OPT_fgnu_unique, options::OPT_fno_gnu_unique,
                         true))
    CC1Args.push_back("-fgnu-unique");

  // An explicit default or protected visibility is an image's boundary, as it
  // is a shared object's on ELF.
  if (!DriverArgs.hasArg(options::OPT_mdefault_visibility_export_mapping_EQ))
    CC1Args.push_back("-mdefault-visibility-export-mapping=explicit");
  DriverArgs.AddLastArg(CC1Args,
                        options::OPT_mdefault_visibility_export_mapping_EQ);

  // Every function is preceded by its type id, whether or not KCFI checks
  // calls. The id ignores pointee types, so that a unit without the checks can
  // be called from one with them, and an option that would compute or encode
  // it otherwise would change every function's ABI. A hot patch overwrites
  // it, which SanitizerArgs reports where KCFI checks calls.
  DriverArgs.ClaimAllArgs(options::OPT_fsanitize_cfi_icall_generalize_pointers);
  CC1Args.push_back("-fsanitize-cfi-icall-generalize-pointers");
  bool KCFI = getSanitizerArgs(DriverArgs, BA, DeviceOffloadKind).hasKCFI();
  for (const Arg *A : DriverArgs.filtered(
           options::OPT_fsanitize_cfi_icall_normalize_integers,
           options::OPT_fsanitize_kcfi_arity,
           options::OPT_fsanitize_kcfi_hash_EQ, options::OPT_fms_hotpatch)) {
    const Option &O = A->getOption();
    if (O.matches(options::OPT_fsanitize_kcfi_hash_EQ)
            ? StringRef(A->getValue()) != "xxHash64"
            : !O.matches(options::OPT_fms_hotpatch) || !KCFI)
      getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
          << A->getAsString(DriverArgs) << getTripleString();
  }
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
  // ABI; its process-wide parts are in clang_rt.wincrt_dynamic's DLL, or with
  // -static in the image. clang_rt.ucrt_memory imports the functions ucrtbase
  // exports and Microsoft declares in vcruntime.
  for (const char *Lib :
       {"wincrt",
        Args.hasArg(options::OPT_static) ? "wincrt_static" : "wincrt_dynamic",
        "ucrt_memory"})
    CmdArgs.push_back(
        Args.MakeArgString("-defaultlib:" + getCompilerRTBasename(Args, Lib)));
  // oldnames maps the POSIX names to the Universal CRT's underscored ones.
  // The API-set umbrella comes last: kernel32 still binds what it covers, and
  // the registry, security, COM and shell-core functions bind to their API
  // sets, not to advapi32, ole32 and shell32, which load msvcrt.dll or
  // connect to win32k. A program connects to win32k only by naming user32,
  // gdi32 or shell32; -mwindows names user32, which every window needs.
  for (const char *Lib : {"-defaultlib:ucrt.lib", "-defaultlib:kernel32.lib",
                          "-defaultlib:ntdll.lib", "-defaultlib:oldnames.lib",
                          "-defaultlib:onecore_apiset.lib"})
    CmdArgs.push_back(Lib);
  if (const Arg *A =
          Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
      A && A->getOption().matches(options::OPT_mwindows))
    CmdArgs.push_back("-defaultlib:user32.lib");
}

/// Returns \p Root's \p Sub directory for the target, then the one every
/// target shares, where they exist.
static SmallVector<std::string, 2>
getTargetAndCommonDirs(const ToolChain &TC, StringRef Root, StringRef Sub) {
  SmallVector<std::string, 2> Dirs;
  SmallString<128> Common(Root), Target;
  llvm::sys::path::append(Common, Sub);
  Target = Common;
  llvm::sys::path::append(Target, TC.getTripleString());
  for (StringRef Dir : {Target.str(), Common.str()})
    if (!Root.empty() && TC.getVFS().exists(Dir))
      Dirs.push_back(Dir.str());
  return Dirs;
}

NTPOSIXToolChain::NTPOSIXToolChain(const Driver &D, const llvm::Triple &Triple,
                                   const ArgList &Args)
    : WindowsItaniumBase(D, Triple, Args) {
  // ntposix.dll's import library and the C library's, in the sysroot.
  llvm::append_range(getFilePaths(),
                     getTargetAndCommonDirs(*this, D.SysRoot, "lib"));
}

void NTPOSIXToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args, BoundArch BA,
    Action::OffloadKind DeviceOffloadKind) const {
  WindowsItaniumBase::addClangTargetOptions(DriverArgs, CC1Args, BA,
                                            DeviceOffloadKind);
  // ntposix.dll is always thread-capable.
  CC1Args.push_back("-pthread");

  // A 16-bit wchar_t alone would bind to the interfaces of the 32-bit one.
  if (const Arg *A = DriverArgs.getLastArg(options::OPT_fshort_wchar,
                                           options::OPT_fno_short_wchar);
      A && A->getOption().matches(options::OPT_fshort_wchar))
    getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
        << A->getAsString(DriverArgs) << getTripleString();
}

void NTPOSIXToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  const Driver &D = getDriver();
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc))
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, D.ResourceDir,
                                  "include");

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // The C library's headers, the target's own first, in the sysroot and beside
  // the toolchain.
  for (const std::string &Dir :
       getTargetAndCommonDirs(*this, D.SysRoot, "include"))
    addSystemInclude(DriverArgs, CC1Args, Dir);
  if (std::optional<std::string> Dir = getStdlibIncludePath())
    addSystemInclude(DriverArgs, CC1Args, *Dir);
}

void NTPOSIXToolChain::addSystemLibArgs(const ArgList &Args,
                                        ArgStringList &CmdArgs) const {
  // ntposix.dll is the runtime and the unwinder, and no image links the
  // Universal CRT or the library of its POSIX names, which objects that MSVC
  // built name.
  CmdArgs.push_back("-defaultlib:ntposix");
  CmdArgs.push_back("-nodefaultlib:ucrt");
  CmdArgs.push_back("-nodefaultlib:oldnames");
}
