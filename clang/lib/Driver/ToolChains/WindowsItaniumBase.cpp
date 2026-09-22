//===--- WindowsItaniumBase.cpp - Shared base for Win+Itanium -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItaniumBase.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void windowsitanium::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                          const InputInfo &Output,
                                          const InputInfoList &Inputs,
                                          const ArgList &Args,
                                          const char *LinkingOutput) const {
  const auto &TC =
      static_cast<const WindowsItaniumBaseToolChain &>(getToolChain());
  const Driver &D = C.getDriver();
  const bool NoStdLib = Args.hasArg(options::OPT_nostdlib);
  const bool NoDefaultLibs = Args.hasArg(options::OPT_nodefaultlibs);
  const bool LinkStartFiles =
      !NoStdLib && !Args.hasArg(options::OPT_nostartfiles);
  const bool LinkDefaultLibs = !NoStdLib && !NoDefaultLibs;
  const bool LinkLibC = LinkDefaultLibs && !Args.hasArg(options::OPT_nolibc);
  ArgStringList CmdArgs;

  // Silence warnings for compiler options on a link-only command line.
  Args.ClaimAllArgs(options::OPT_g_Group);
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  Args.ClaimAllArgs(options::OPT_w);
  Args.ClaimAllArgs(options::OPT_stdlib_EQ);

  // lld-link is the only linker that implements the import model.
  if (const Arg *A = Args.getLastArg(options::OPT_fuse_ld_EQ)) {
    StringRef Linker = A->getValue();
    if (!Linker.equals_insensitive("lld") &&
        !Linker.equals_insensitive("lld-link")) {
      D.Diag(diag::err_drv_unsupported_opt_for_target)
          << A->getAsString(Args) << TC.getTriple().str();
      return;
    }
  }

  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(std::string("-out:") + Output.getFilename()));

  if (Args.hasArg(options::OPT_marm64x))
    CmdArgs.push_back("-machine:arm64x");
  else if (TC.getTriple().isWindowsArm64EC())
    CmdArgs.push_back("-machine:arm64ec");
  else if (TC.getArch() == llvm::Triple::x86)
    CmdArgs.push_back("-machine:x86");
  else if (TC.getArch() == llvm::Triple::aarch64)
    CmdArgs.push_back("-machine:arm64");
  else
    CmdArgs.push_back("-machine:x64");

  bool IsDLL = Args.hasArg(options::OPT__SLASH_LD, options::OPT__SLASH_LDd,
                           options::OPT_shared);
  if (IsDLL) {
    CmdArgs.push_back("-dll");

    SmallString<128> ImplibName(Output.getFilename());
    llvm::sys::path::replace_extension(ImplibName, "dll.lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImplibName));

    // The x86 entry point is decorated as __stdcall.
    CmdArgs.push_back(TC.getArch() == llvm::Triple::x86
                          ? "-entry:_DllMainCRTStartup@12"
                          : "-entry:_DllMainCRTStartup");
  } else {
    const Arg *A =
        Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
    if (A && A->getOption().matches(options::OPT_mwindows))
      CmdArgs.push_back("-subsystem:windows");
    else
      CmdArgs.push_back("-subsystem:console");
    if (LinkStartFiles)
      CmdArgs.push_back(
          Args.MakeArgString("-entry:" + TC.getExecutableEntryPoint(Args)));
  }

  if (const Arg *A = Args.getLastArg(options::OPT_fveclib))
    if (StringRef(A->getValue()) == "ArmPL")
      CmdArgs.push_back(Args.MakeArgString("--dependent-lib=amath"));

  TC.addSystemLinkArgs(Args, CmdArgs, IsDLL);

  for (const auto &LibPath : Args.getAllArgValues(options::OPT_L))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));
  TC.AddRuntimeLibSearchPaths(Args, CmdArgs);
  CmdArgs.push_back("-nologo");

  if (LinkStartFiles && !TC.addStartFiles(Args, CmdArgs, IsDLL))
    return;

  // The runtime libraries are default libraries: lld searches them after
  // every positional input, so a definition in the user's objects or
  // libraries takes precedence over the same symbol in a runtime archive,
  // as it does with link.exe and the MSVC driver.
  if (LinkDefaultLibs) {
    // clang-cl has no C-only mode, so it links the C++ standard library
    // whenever the inputs may be C++. lld pulls members only when they are
    // referenced, so a C program gains no dependency on it.
    if (TC.ShouldLinkCXXStdlib(Args) ||
        (D.IsCLMode() && !Args.hasArg(options::OPT_nostdlibxx)))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);

    if (TC.GetUnwindLibType(Args) == ToolChain::UNW_CompilerRT)
      CmdArgs.push_back("-defaultlib:libunwind.dll.lib");
    else if (const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ))
      D.Diag(diag::err_drv_unsupported_unwind_for_platform)
          << A->getValue() << TC.getTriple().normalize();

    CmdArgs.push_back(Args.MakeArgString(
        Twine("-defaultlib:") + TC.getCompilerRTArgString(Args, "builtins")));

    if (LinkLibC && !TC.addLibCArgs(Args, CmdArgs))
      return;
  }

  // Static data that holds the address of a symbol from another DLL is
  // filled by the loader through an import descriptor of its own. A
  // definition in the link takes precedence over an import library's entry
  // for the same name.
  CmdArgs.push_back("-import-slots");

  if (!NoDefaultLibs)
    TC.addNoDefaultLibArgs(Args, CmdArgs);

  const SanitizerArgs &Sanitize = TC.getSanitizerArgs(Args);
  if (Sanitize.needsFuzzer()) {
    if (!Args.hasArg(options::OPT_shared))
      CmdArgs.push_back(Args.MakeArgString(
          Twine("-wholearchive:") + TC.getCompilerRTArgString(Args, "fuzzer")));
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
  }
  if (Sanitize.needsAsanRt()) {
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
    CmdArgs.push_back(TC.getCompilerRTArgString(Args, "asan_dynamic"));
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-wholearchive:") +
        TC.getCompilerRT(Args, "asan_dynamic_runtime_thunk")));
    // Prevent the ASan SEH interceptor from being discarded.
    CmdArgs.push_back(TC.getArch() == llvm::Triple::x86
                          ? "-include:___asan_seh_interceptor"
                          : "-include:__asan_seh_interceptor");
  }

  if (D.isUsingLTO()) {
    if (Arg *A = tools::getLastProfileSampleUseArg(Args))
      CmdArgs.push_back(
          Args.MakeArgString(Twine("-lto-sample-profile:") + A->getValue()));
    if (Args.hasFlag(options::OPT_gsplit_dwarf, options::OPT_gno_split_dwarf,
                     false))
      CmdArgs.push_back(Args.MakeArgString(Twine("-dwodir:") +
                                           Output.getFilename() + "_dwo"));
  }

  if (!Args.hasFlag(options::OPT_mincremental_linker_compatible,
                    options::OPT_mno_incremental_linker_compatible, true))
    CmdArgs.push_back("-Brepro");

  if (Args.hasArg(options::OPT_fms_hotpatch, options::OPT__SLASH_hotpatch))
    CmdArgs.push_back("-functionpadmin");

  TC.addGuardLinkArgs(Args, CmdArgs, IsDLL);

  if (Args.hasArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    CmdArgs.push_back("-debug");

  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);

  // lld-link has no -l; NormalizeLLDLinkArgs resolves each library once every
  // search path is known.
  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l)) {
      CmdArgs.push_back("-l");
      CmdArgs.push_back(A.getValue());
      continue;
    }
    A.renderAsInput(Args, CmdArgs);
  }

  if (!TC.addPostInputLibs(Args, Inputs, CmdArgs))
    return;

  TC.addOffloadRTLibs(C.getActiveOffloadKinds(), Args, CmdArgs);
  TC.addProfileRTLibs(Args, CmdArgs);
  TC.NormalizeLLDLinkArgs(Args, CmdArgs);

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(TC.GetProgramPath("lld-link")), CmdArgs, Inputs,
      Output));
}

WindowsItaniumBaseToolChain::WindowsItaniumBaseToolChain(
    const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : ToolChain(D, Triple, Args), CudaInstallation(D, Triple, Args),
      RocmInstallation(D, Triple, Args), SYCLInstallation(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);

  SmallString<128> LibPath(D.Dir);
  llvm::sys::path::append(LibPath, "..", "lib");
  if (getVFS().exists(LibPath))
    getFilePaths().push_back(std::string(LibPath));

  SmallString<128> TargetLibPath(D.Dir);
  llvm::sys::path::append(TargetLibPath, "..", "lib", Triple.str());
  if (getVFS().exists(TargetLibPath))
    getFilePaths().push_back(std::string(TargetLibPath));
}

ToolChain::UnwindTableLevel
WindowsItaniumBaseToolChain::getDefaultUnwindTableLevel(
    const ArgList &Args) const {
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::arm ||
      getArch() == llvm::Triple::thumb || getArch() == llvm::Triple::aarch64)
    return UnwindTableLevel::Asynchronous;
  return UnwindTableLevel::None;
}

bool WindowsItaniumBaseToolChain::isPICDefault() const {
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

bool WindowsItaniumBaseToolChain::isPIEDefault(const ArgList &Args) const {
  return false;
}

bool WindowsItaniumBaseToolChain::isPICDefaultForced() const {
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

SanitizerMask WindowsItaniumBaseToolChain::getSupportedSanitizers() const {
  SanitizerMask Res = ToolChain::getSupportedSanitizers();
  Res |= SanitizerKind::Address;
  Res |= SanitizerKind::PointerCompare;
  Res |= SanitizerKind::PointerSubtract;
  Res |= SanitizerKind::Fuzzer;
  Res |= SanitizerKind::FuzzerNoLink;
  Res &= ~SanitizerKind::CFIMFCall;
  return Res;
}

llvm::ExceptionHandling
WindowsItaniumBaseToolChain::GetExceptionModel(const ArgList &Args) const {
  if (Args.hasArg(options::OPT_fsjlj_exceptions))
    return llvm::ExceptionHandling::SjLj;
  // SEH with Itanium personality on 64-bit; SJLJ on 32-bit.
  // Table-based SEH (DISPATCHER_CONTEXT) only exists on x64/ARM64.
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::aarch64)
    return llvm::ExceptionHandling::WinEH;
  return llvm::ExceptionHandling::SjLj;
}

ToolChain::CXXStdlibType
WindowsItaniumBaseToolChain::GetCXXStdlibType(const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++") {
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

void WindowsItaniumBaseToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind /*DeviceOffloadKind*/) const {
  // Avoid LTO link errors from available_externally dllimport inlines.
  if (!DriverArgs.hasFlag(options::OPT__SLASH_Zc_dllexportInlines,
                          options::OPT_fno_dllexport_inlines, false))
    CC1Args.push_back("-fno-dllexport-inlines");

  // An explicit default visibility marks the shared-library boundary in both
  // directions: a marked definition is exported and a marked declaration is
  // imported. A function that the translation unit does not define is called
  // through the import table, which the linker binds directly when the
  // definition is in the image. Unmarked data is local unless the user asks
  // for auto-import.
  if (!DriverArgs.hasArg(options::OPT_mdefault_visibility_export_mapping_EQ))
    CC1Args.push_back("-mdefault-visibility-export-mapping=explicit");
  DriverArgs.AddLastArg(CC1Args,
                        options::OPT_mdefault_visibility_export_mapping_EQ);
  if (!DriverArgs.hasArg(options::OPT_fauto_import,
                         options::OPT_fno_auto_import))
    CC1Args.push_back("-fno-auto-import");
  if (DriverArgs.hasFlag(options::OPT_fno_plt, options::OPT_fplt,
                         getArch() == llvm::Triple::x86_64))
    CC1Args.push_back("-fno-plt");

  // clang-cl translates /guard: together with its other options.
  if (!getDriver().IsCLMode()) {
    GuardOptions Guard = getGuardOptions(DriverArgs);
    if (Guard.Checks)
      CC1Args.push_back("-cfguard");
    else if (Guard.Tables)
      CC1Args.push_back("-cfguard-no-checks");
    if (Guard.EHCont && !isCETCompatible())
      CC1Args.push_back("-ehcontguard");
  }
  // Every object records its EH continuation targets, so that the table of
  // an image linked with Control Flow Guard is complete whichever objects it
  // combines. Only SEH handlers have targets; the others emit nothing.
  if (isCETCompatible())
    CC1Args.push_back("-ehcontguard");
}

WindowsItaniumBaseToolChain::GuardOptions
WindowsItaniumBaseToolChain::getGuardOptions(const ArgList &Args) const {
  GuardOptions Guard;
  for (const Arg *A :
       Args.filtered(options::OPT_mguard_EQ, options::OPT__SLASH_guard)) {
    A->claim();
    StringRef Value = A->getValue();
    if (Value.equals_insensitive("cf")) {
      Guard.Tables = Guard.Checks = true;
    } else if (Value.equals_insensitive("cf-nochecks") ||
               Value.equals_insensitive("cf,nochecks")) {
      Guard.Tables = true;
      Guard.Checks = false;
    } else if (Value.equals_insensitive("cf-")) {
      Guard.Tables = Guard.Checks = false;
    } else if (Value.equals_insensitive("ehcont")) {
      Guard.EHCont = true;
    } else if (Value.equals_insensitive("ehcont-")) {
      Guard.EHCont = false;
    } else if (Value.equals_insensitive("none")) {
      Guard = GuardOptions();
    } else {
      getDriver().Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << Value;
    }
  }
  return Guard;
}

void WindowsItaniumBaseToolChain::addGuardLinkArgs(const ArgList &Args,
                                                   ArgStringList &CmdArgs,
                                                   bool IsDLL) const {
  if (isCETCompatible())
    CmdArgs.push_back("-cetcompat");

  GuardOptions Guard = getGuardOptions(Args);
  if (isCETCompatible())
    Guard.EHCont |= Guard.Tables;
  if (!Guard.Tables && !Guard.EHCont)
    return;

  // lld-link honours the last -guard: argument only, so every mode goes into
  // one. Export suppression narrows an executable's valid indirect-call
  // targets to the exports whose addresses are taken.
  SmallVector<StringRef, 3> Modes;
  if (Guard.Tables)
    Modes.push_back("cf");
  if (Guard.EHCont)
    Modes.push_back("ehcont");
  if (Guard.Tables && !IsDLL)
    Modes.push_back("exportsuppress");
  CmdArgs.push_back(Args.MakeArgString("-guard:" + llvm::join(Modes, ",")));
}

Tool *WindowsItaniumBaseToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

void WindowsItaniumBaseToolChain::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  DriverArgs.getLastArg(options::OPT_stdlib_EQ);

  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdincxx,
                        options::OPT_nostdlibinc))
    return;

  const Driver &D = getDriver();

  // Target-specific path for multi-target installations.
  SmallString<128> TargetPath(D.Dir);
  llvm::sys::path::append(TargetPath, "..", "include", getTripleString());
  llvm::sys::path::append(TargetPath, "c++", "v1");
  if (D.getVFS().exists(TargetPath))
    addSystemInclude(DriverArgs, CC1Args, TargetPath);

  SmallString<128> InstallPath(D.Dir);
  llvm::sys::path::append(InstallPath, "..", "include", "c++", "v1");
  if (D.getVFS().exists(InstallPath))
    addSystemInclude(DriverArgs, CC1Args, InstallPath);

  for (const std::string &LibPath : getFilePaths()) {
    SmallString<128> LibIncludePath(LibPath);
    llvm::sys::path::append(LibIncludePath, "..", "include", "c++", "v1");
    if (D.getVFS().exists(LibIncludePath)) {
      addSystemInclude(DriverArgs, CC1Args, LibIncludePath);
      break;
    }
  }

  if (!D.SysRoot.empty()) {
    SmallString<128> SysrootPath(D.SysRoot);
    llvm::sys::path::append(SysrootPath, "include", "c++", "v1");
    if (D.getVFS().exists(SysrootPath))
      addSystemInclude(DriverArgs, CC1Args, SysrootPath);
  }
}

void WindowsItaniumBaseToolChain::AddCXXStdlibLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-defaultlib:libc++.dll.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-defaultlib:libc++experimental.lib");
}

const char *WindowsItaniumBaseToolChain::GetLibraryArg(
    const ArgList &Args, const ArgStringList &CmdArgs, StringRef Name) const {
  // Exact filenames bypass the platform's prefix/suffix search.
  if (Name.consume_front(":"))
    return Args.MakeArgString(Name);
  if (Name.ends_with_insensitive(".lib") || Name.ends_with_insensitive(".a"))
    return Args.MakeArgString(Name);

  // lld-link does not implement -l lookup. Search each linker directory in
  // order, preferring an import library over a static archive. Unprefixed
  // libraries remain available for Windows SDK and other native libraries.
  const std::string Names[] = {("lib" + Name + ".dll.lib").str(),
                               ("lib" + Name + ".lib").str(),
                               (Name + ".lib").str()};
  for (const auto &File : Names) {
    if (getVFS().exists(File))
      return Args.MakeArgString(File);
  }
  for (StringRef Arg : CmdArgs) {
    if (!Arg.consume_front_insensitive("-libpath:") &&
        !Arg.consume_front_insensitive("/libpath:"))
      continue;
    for (const auto &File : Names) {
      SmallString<128> Path(Arg);
      llvm::sys::path::append(Path, File);
      if (getVFS().exists(Path))
        return Args.MakeArgString(Path);
    }
  }
  // Preserve lld-link's diagnostic and native library search on a miss.
  return Args.MakeArgString(Names[2]);
}

void WindowsItaniumBaseToolChain::AddRuntimeLibSearchPaths(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  auto AddLibPath = [&](const std::string &LibPath) {
    if (getVFS().exists(LibPath))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));
  };

  for (const std::string &LibPath : getFilePaths())
    AddLibPath(LibPath);

  for (const std::string &LibPath : getLibraryPaths())
    AddLibPath(LibPath);

  std::string CRTPath = getCompilerRTPath();
  if (getVFS().exists(CRTPath))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + CRTPath));
}

void WindowsItaniumBaseToolChain::NormalizeLLDLinkArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  // -rdynamic exports every default-visibility definition on ELF. An image
  // here exports the definitions its sources mark with default visibility
  // without it, and an unmarked one when compiled with
  // -mdefault-visibility-export-mapping=all, so the linker needs nothing.
  Args.ClaimAllArgs(options::OPT_rdynamic);

  for (auto It = CmdArgs.begin(); It != CmdArgs.end();) {
    StringRef Value(*It);
    // Resolve -l only after collecting every linker argument, including a
    // -Wl,/libpath: that follows the library on the command line.
    if (Value == "-l" && It + 1 != CmdArgs.end()) {
      *It = GetLibraryArg(Args, CmdArgs, *(It + 1));
      It = CmdArgs.erase(It + 1);
      continue;
    }
    // PE has no run-time library search path. Drop the ELF options that
    // build systems written for ELF pass, with their separate values.
    if (Value == "-rpath" || Value == "-rpath-link") {
      It = CmdArgs.erase(It);
      if (It != CmdArgs.end())
        It = CmdArgs.erase(It);
      continue;
    }
    if (Value.starts_with("-rpath=") || Value.starts_with("-rpath-link=")) {
      It = CmdArgs.erase(It);
      continue;
    }
    ++It;
  }
}

void WindowsItaniumBaseToolChain::AddSystemIncludeWithSubfolder(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    const std::string &Folder, const Twine &Sub1, const Twine &Sub2,
    const Twine &Sub3) const {
  llvm::SmallString<128> P(Folder);
  llvm::sys::path::append(P, Sub1, Sub2, Sub3);
  addSystemInclude(DriverArgs, CC1Args, P);
}

void WindowsItaniumBaseToolChain::AddCudaIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  CudaInstallation->AddCudaIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::AddHIPIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  RocmInstallation->AddHIPIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::addSYCLIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  SYCLInstallation->addSYCLIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumBaseToolChain::addOffloadRTLibs(
    unsigned ActiveKinds, const ArgList &Args, ArgStringList &CmdArgs) const {
  if (Args.hasArg(options::OPT_no_hip_rt) || Args.hasArg(options::OPT_r))
    return;
  if (ActiveKinds & Action::OFK_HIP) {
    CmdArgs.append({Args.MakeArgString(StringRef("-libpath:") +
                                       RocmInstallation->getLibPath()),
                    "amdhip64.lib"});
  }
}

void WindowsItaniumBaseToolChain::printVerboseInfo(raw_ostream &OS) const {
  CudaInstallation->print(OS);
  RocmInstallation->print(OS);
}
