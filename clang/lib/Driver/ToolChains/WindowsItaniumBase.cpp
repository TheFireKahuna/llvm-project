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
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

// lld-link looks a library up by the name it is given. The runtimes of these
// targets carry the "lib" prefix, as on ELF, and the Windows SDK libraries do
// not, so -lname finds libname.dll.lib, then libname.lib, in the library
// directories, and otherwise leaves name.lib to lld-link's own search.
static const char *getLibraryArg(const ToolChain &TC, const ArgList &Args,
                                 ArrayRef<std::string> LibDirs,
                                 StringRef Name) {
  if (Name.ends_with_insensitive(".lib"))
    return Args.MakeArgString(Name);
  const std::string Files[] = {("lib" + Name + ".dll.lib").str(),
                               ("lib" + Name + ".lib").str()};
  for (const std::string &Dir : LibDirs) {
    for (const std::string &File : Files) {
      SmallString<128> Path(Dir);
      llvm::sys::path::append(Path, File);
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
  const auto &TC = static_cast<const toolchains::WindowsItaniumBaseToolChain &>(
      getToolChain());
  const Driver &D = TC.getDriver();
  ArgStringList CmdArgs;

  // The link line uses options only lld-link has, such as -lldignoreenv.
  if (const Arg *A = Args.getLastArg(options::OPT_fuse_ld_EQ)) {
    StringRef Linker = A->getValue();
    if (!Linker.equals_insensitive("lld") &&
        !Linker.equals_insensitive("lld-link"))
      D.Diag(diag::err_drv_unsupported_opt_for_target)
          << A->getAsString(Args) << TC.getTriple().str();
  }

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

  // The import binding model of these targets.
  CmdArgs.push_back("-import-slots");

  // On x86-64 Windows Itanium, the image runs with the hardware shadow stack,
  // and lld-link then gives it the table of EH continuation targets.
  if (TC.getTriple().isWindowsItaniumEnvironment() &&
      TC.getArch() == llvm::Triple::x86_64)
    CmdArgs.push_back("-cetcompat");

  bool IsDLL = Args.hasArg(options::OPT_shared, options::OPT__SLASH_LD,
                           options::OPT__SLASH_LDd);
  if (IsDLL) {
    CmdArgs.push_back("-dll");
    // name.dll.lib, so that the import library of name.dll and the static
    // archive name.lib can sit in one directory.
    SmallString<128> ImplibName(Output.getFilename());
    llvm::sys::path::replace_extension(ImplibName, "dll.lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImplibName));
  } else if (const Arg *A = Args.getLastArg(options::OPT_mwindows,
                                            options::OPT_mconsole)) {
    CmdArgs.push_back(A->getOption().matches(options::OPT_mwindows)
                          ? "-subsystem:windows"
                          : "-subsystem:console");
  }

  if (Args.hasArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    CmdArgs.push_back("-debug");

  // Control Flow Guard is on unless -mguard=none, and an executable suppresses
  // its exports as call targets until GetProcAddress returns them. lld-link
  // honors only the last -guard: option, so the two go together.
  if (TC.getGuardMode(Args) != "none")
    CmdArgs.push_back(IsDLL ? "-guard:cf" : "-guard:cf,exportsuppress");

  std::vector<std::string> LibDirs = Args.getAllArgValues(options::OPT_L);
  TC.addSystemLibraryDirs(Args, LibDirs);
  for (const std::string &Dir : TC.getFilePaths())
    LibDirs.push_back(Dir);
  for (const std::string &Dir : TC.getLibraryPaths())
    if (TC.getVFS().exists(Dir))
      LibDirs.push_back(Dir);
  std::string CRTPath = TC.getCompilerRTPath();
  if (TC.getVFS().exists(CRTPath))
    LibDirs.push_back(CRTPath);
  for (const std::string &Dir : LibDirs)
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + Dir));

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles))
    TC.addStartFiles(Args, CmdArgs, IsDLL);

  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l)) {
      CmdArgs.push_back(getLibraryArg(TC, Args, LibDirs, A.getValue()));
      continue;
    }
    A.renderAsInput(Args, CmdArgs);
  }

  // The runtime libraries are default libraries, which lld-link searches after
  // every input, so that a definition in the user's objects or libraries
  // takes precedence, and a user's -nodefaultlib: still removes them.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    // clang-cl has no C-only mode, so it links libc++ whenever the inputs may
    // be C++; a C program takes nothing from it.
    if (TC.ShouldLinkCXXStdlib(Args) ||
        (D.IsCLMode() && !Args.hasArg(options::OPT_nostdlibxx)))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);
    if (TC.GetUnwindLibType(Args) == ToolChain::UNW_CompilerRT)
      TC.addUnwindLibArgs(Args, CmdArgs);
    CmdArgs.push_back(Args.MakeArgString(
        "-defaultlib:" + TC.getCompilerRTBasename(Args, "builtins")));
    if (!Args.hasArg(options::OPT_nolibc))
      TC.addSystemLibArgs(Args, CmdArgs);
    TC.addNoDefaultLibArgs(Args, CmdArgs);
  }

  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);
  TC.addProfileRTLibs(Args, CmdArgs);

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(TC.GetProgramPath("lld-link")), CmdArgs, Inputs,
      Output));
}

WindowsItaniumBaseToolChain::WindowsItaniumBaseToolChain(
    const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : ToolChain(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);

  // The runtimes installed beside the toolchain, when they are not in a
  // per-target directory.
  SmallString<128> LibPath(D.Dir);
  llvm::sys::path::append(LibPath, "..", "lib");
  if (getVFS().exists(LibPath))
    getFilePaths().push_back(std::string(LibPath));
}

Tool *WindowsItaniumBaseToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

ToolChain::UnwindLibType
WindowsItaniumBaseToolChain::GetUnwindLibType(const ArgList &Args) const {
  // libunwind is the platform's unwind library.
  const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ);
  if (!A || StringRef(A->getValue()) == "platform")
    return ToolChain::UNW_CompilerRT;
  return ToolChain::GetUnwindLibType(Args);
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

  // A variable is imported when its declaration says so, not on the chance
  // that a DLL provides it, unless -fauto-import asks for that.
  if (!DriverArgs.hasFlag(options::OPT_fauto_import,
                          options::OPT_fno_auto_import, false))
    CC1Args.push_back("-fno-auto-import");

  // On x86-64 a call to a function the translation unit does not define goes
  // through the import table, as it goes through the GOT under -fno-plt on
  // ELF, and the linker makes it direct when the function is in the image.
  // AArch64 calls directly unless -fno-plt is given.
  if (DriverArgs.hasFlag(options::OPT_fno_plt, options::OPT_fplt,
                         getArch() == llvm::Triple::x86_64))
    CC1Args.push_back("-fno-plt");

  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole})
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();

  // Every function carries a KCFI prefix with the marker, whether or not
  // KCFI checks calls, and its type ignores pointee types, so that units
  // built with -fno-sanitize=kcfi can be called from units that check.
  // SanitizerArgs passes the options that change the type only with the
  // checks.
  CC1Args.push_back("-fsanitize-kcfi-marker");
  bool HasKCFI = getSanitizerArgs(DriverArgs).hasKCFI();
  bool Generalize =
      DriverArgs.hasArg(options::OPT_fsanitize_cfi_icall_generalize_pointers);
  if (!HasKCFI || !Generalize)
    CC1Args.push_back("-fsanitize-cfi-icall-generalize-pointers");
  if (!HasKCFI) {
    if (DriverArgs.hasArg(options::OPT_fsanitize_cfi_icall_normalize_integers))
      CC1Args.push_back("-fsanitize-cfi-icall-experimental-normalize-integers");
    if (const Arg *A =
            DriverArgs.getLastArg(options::OPT_fsanitize_kcfi_hash_EQ))
      CC1Args.push_back(DriverArgs.MakeArgString(
          Twine("-fsanitize-kcfi-hash=") + A->getValue()));
  }

  // A hot patch writes a jump into the bytes before a function's entry,
  // which hold its KCFI prefix. Without KCFI checks, SanitizerArgs does not
  // reject it.
  if (const Arg *A = DriverArgs.getLastArg(options::OPT_fms_hotpatch);
      A && !HasKCFI)
    getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
        << A->getAsString(DriverArgs) << getTriple().str();

  StringRef GuardArgs = getGuardMode(DriverArgs);
  if (GuardArgs == "cf") {
    // Emit CFG instrumentation and the table of address-taken functions.
    CC1Args.push_back("-cfguard");
  } else if (GuardArgs == "cf-nochecks") {
    // Emit only the table of address-taken functions.
    CC1Args.push_back("-cfguard-no-checks");
  }
}

StringRef WindowsItaniumBaseToolChain::getGuardMode(const ArgList &Args) const {
  const Arg *A = Args.getLastArg(options::OPT_mguard_EQ);
  if (!A)
    return "cf";
  StringRef GuardArgs = A->getValue();
  if (GuardArgs != "none" && GuardArgs != "cf" && GuardArgs != "cf-nochecks") {
    getDriver().Diag(diag::err_drv_unsupported_option_argument)
        << A->getSpelling() << GuardArgs;
    return "none";
  }
  return GuardArgs;
}

void WindowsItaniumBaseToolChain::AddCXXStdlibLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-defaultlib:libc++.dll.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-defaultlib:libc++experimental.lib");
}

void WindowsItaniumBaseToolChain::addUnwindLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-defaultlib:libunwind.dll.lib");
}

void WindowsItaniumBaseToolChain::addNoDefaultLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  // Objects compiled by clang-cl or MSVC name their C runtime; these targets
  // link none of the Visual C++ ones.
  for (const char *Lib : {"msvcrt", "msvcrtd", "vcruntime", "vcruntimed",
                          "libcmt", "libcmtd", "ucrtd"})
    CmdArgs.push_back(Args.MakeArgString("-nodefaultlib:" + Twine(Lib)));
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

  // clang-cl's /guard: spellings select the -mguard= mode, which is otherwise
  // on by default: /guard:cf- turns Control Flow Guard off. /guard:ehcont and
  // /guard:ehcont- change nothing, since whether objects carry EH continuation
  // metadata is the target's choice.
  const Arg *GuardArg = nullptr;
  StringRef GuardMode;
  for (Arg *A : Args.filtered(options::OPT__SLASH_guard)) {
    StringRef Value = A->getValue();
    if (Value.equals_insensitive("cf")) {
      GuardArg = A;
      GuardMode = "cf";
    } else if (Value.equals_insensitive("cf,nochecks")) {
      GuardArg = A;
      GuardMode = "cf-nochecks";
    } else if (Value.equals_insensitive("cf-")) {
      GuardArg = A;
      GuardMode = "none";
    } else if (!Value.equals_insensitive("ehcont") &&
               !Value.equals_insensitive("ehcont-")) {
      getDriver().Diag(diag::err_drv_invalid_value)
          << A->getSpelling() << Value;
    }
    A->claim();
  }
  DAL.eraseArg(options::OPT__SLASH_guard);
  if (GuardArg) {
    // /d2guardnochecks keeps only the table of address-taken functions.
    if (GuardMode == "cf" && Args.hasArg(options::OPT__SLASH_d2guardnochecks))
      GuardMode = "cf-nochecks";
    DAL.AddJoinedArg(GuardArg, Opts.getOption(options::OPT_mguard_EQ),
                     GuardMode);
  }

  // clang-cl's /GS- only leaves out the stack protector clang-cl asks for,
  // which would leave the toolchain's default in place.
  if (const Arg *A = Args.getLastArgNoClaim(
          options::OPT__SLASH_GS, options::OPT__SLASH_GS_,
          options::OPT_fstack_protector, options::OPT_fstack_protector_strong,
          options::OPT_fstack_protector_all, options::OPT_fno_stack_protector);
      A && A->getOption().matches(options::OPT__SLASH_GS_))
    DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_stack_protector));

  // Every function and every data item has a section of its own, from which
  // the linker takes its extent, so the -fno- forms, and clang-cl's /Gy- and
  // /Gw-, are ignored.
  for (Arg *A : Args.filtered(options::OPT_fno_function_sections,
                              options::OPT_fno_data_sections)) {
    getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
        << A->getAsString(Args) << getTriple().str();
    A->claim();
    DAL.eraseArg(A->getOption().getID());
  }

  // Without an unwind table, the unwinder takes a function for a leaf, so an
  // exception, a longjmp or a stack walk through it would go wrong. The tables
  // are always emitted, even with -ffreestanding.
  for (Arg *A : Args.filtered(options::OPT_fno_asynchronous_unwind_tables,
                              options::OPT_fno_unwind_tables)) {
    getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
        << A->getAsString(Args) << getTriple().str();
    A->claim();
    DAL.eraseArg(A->getOption().getID());
  }
  DAL.AddFlagArg(nullptr,
                 Opts.getOption(options::OPT_fasynchronous_unwind_tables));

  for (Arg *A : Args.filtered(options::OPT_fsjlj_exceptions,
                              options::OPT_fdwarf_exceptions,
                              options::OPT_fwasm_exceptions)) {
    getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
        << A->getAsString(Args) << getTriple().str();
    DAL.eraseArg(A->getOption().getID());
    DAL.AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
  }
}
