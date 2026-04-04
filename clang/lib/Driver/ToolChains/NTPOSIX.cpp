//===--- NTPOSIX.cpp - NT-POSIX ToolChain ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NTPOSIX.h"
#include "Clang.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
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

// ============================================================================
// NTPOSIXToolChain
// ============================================================================

NTPOSIXToolChain::NTPOSIXToolChain(const Driver &D,
                                   const llvm::Triple &Triple,
                                   const ArgList &Args)
    : WindowsItaniumBaseToolChain(D, Triple, Args) {
  // Sysroot-based lib paths for llvm-libc.
  if (!D.SysRoot.empty()) {
    SmallString<128> SysrootLib(D.SysRoot);
    llvm::sys::path::append(SysrootLib, "lib");
    if (getVFS().exists(SysrootLib))
      getFilePaths().push_back(std::string(SysrootLib));

    SmallString<128> SysrootTargetLib(D.SysRoot);
    llvm::sys::path::append(SysrootTargetLib, "lib", Triple.str());
    if (getVFS().exists(SysrootTargetLib))
      getFilePaths().push_back(std::string(SysrootTargetLib));
  }
}

void NTPOSIXToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind /*DeviceOffloadKind*/) const {

  // Dual-mode libc: by default consumer TUs see c.dll-resident symbols as
  // __declspec(dllimport) via LIBC_API / __LIBC_DATA_IMPORT / __LIBC_FUNC_IMPORT
  // in libc headers. -static-libc strips the annotation so libc.lib static
  // linkage resolves plain externs directly. _LIBC_DLL is never set when
  // LIBC_FULL_BUILD is active (that flag is the build-system's responsibility
  // and signals "inside libc itself" — LIBC_API flips to dllexport there).
  if (!DriverArgs.hasArg(options::OPT_static_libc)) {
    CC1Args.push_back("-D_LIBC_DLL");
  }

  // Avoid LTO link errors from available_externally dllimport inlines.
  if (!DriverArgs.hasArg(options::OPT_fno_dllexport_inlines))
    CC1Args.push_back("-fno-dllexport-inlines");

  // POSIX-compliant wchar_t: 32-bit signed int (UTF-32).
  if (!DriverArgs.hasArg(options::OPT_fshort_wchar,
                         options::OPT_fno_short_wchar)) {
    CC1Args.push_back("-fwchar-type=int");
    CC1Args.push_back("-fsigned-wchar");
  }

  // Enable POSIX thread model.
  if (!DriverArgs.hasArg(options::OPT_fno_threadsafe_statics))
    CC1Args.push_back("-pthread");

  // Disable the SysV 128-byte red zone by default. The NT kernel's user-mode
  // dispatch paths (KiUserExceptionDispatcher for hardware faults,
  // KiUserApcDispatcher for APC delivery) stage their frames at [rsp..] and
  // do not respect the SysV red zone — a leaf function with live locals in
  // [rsp-128..rsp-1] would see them silently corrupted when a VEH filter
  // returns EXCEPTION_CONTINUE_EXECUTION or when a POSIX signal is delivered
  // mid-leaf via APC. The perf cost (one rsp adjust per leaf prologue/epilogue)
  // is small and strictly bounded; the corruption risk is unbounded. Users
  // who understand the constraint can still pass -mred-zone to opt back in.
  if (!DriverArgs.hasArg(options::OPT_mred_zone, options::OPT_mno_red_zone))
    CC1Args.push_back("-disable-red-zone");

  // Control Flow Guard support.
  if (Arg *A = DriverArgs.getLastArg(options::OPT_mguard_EQ)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs == "cf")
      CC1Args.push_back("-cfguard");
    else if (GuardArgs == "cf-nochecks")
      CC1Args.push_back("-cfguard-no-checks");
    else if (GuardArgs != "none")
      getDriver().Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << GuardArgs;
  }

  // Claim linker-only options to suppress warnings.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole}) {
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();
  }
}

void NTPOSIXToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  // Clang resource headers.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include", "", "");
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // llvm-libc headers from sysroot and driver-relative paths.
  auto AddLibCIncludes = [&](const llvm::Twine &Root) {
    llvm::SmallString<128> TargetInclude(Root.str());
    llvm::sys::path::append(TargetInclude, "include", getTripleString());
    if (getVFS().exists(TargetInclude))
      addSystemInclude(DriverArgs, CC1Args, TargetInclude);

    llvm::SmallString<128> GenericInclude(Root.str());
    llvm::sys::path::append(GenericInclude, "include");
    if (getVFS().exists(GenericInclude))
      addSystemInclude(DriverArgs, CC1Args, GenericInclude);
  };

  if (!getDriver().SysRoot.empty())
    AddLibCIncludes(getDriver().SysRoot);

  llvm::SmallString<128> DriverRoot(getDriver().Dir);
  llvm::sys::path::append(DriverRoot, "..");
  AddLibCIncludes(DriverRoot);

  for (const std::string &LibPath : getFilePaths()) {
    llvm::SmallString<128> Root(LibPath);
    llvm::sys::path::append(Root, "..");
    AddLibCIncludes(Root);
  }

  // No Windows SDK includes — llvm-libc provides all system headers.
}

Tool *NTPOSIXToolChain::buildLinker() const {
  return new tools::ntposix::Linker(*this);
}

// ============================================================================
// Linker
// ============================================================================

void ntposix::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                   const InputInfo &Output,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   const char *LinkingOutput) const {
  ArgStringList CmdArgs;

  auto &TC = static_cast<const NTPOSIXToolChain &>(getToolChain());
  const Driver &D = C.getDriver();
  const bool NoStdLib = Args.hasArg(options::OPT_nostdlib);
  const bool NoStartFiles = Args.hasArg(options::OPT_nostartfiles);
  const bool NoDefaultLibs = Args.hasArg(options::OPT_nodefaultlibs);
  const bool NoLibC = Args.hasArg(options::OPT_nolibc);
  const bool LinkStartFiles = !NoStdLib && !NoStartFiles;
  const bool LinkDefaultLibs = !NoStdLib && !NoDefaultLibs;
  const bool LinkLibC = LinkDefaultLibs && !NoLibC;
  bool HasExplicitLibC = false;

  auto AddRequiredFile = [&](const char *Name) -> bool {
    std::string Path = TC.GetFilePath(Name);
    if (!TC.getVFS().exists(Path)) {
      D.Diag(diag::err_drv_no_such_file) << Name;
      return false;
    }
    CmdArgs.push_back(Args.MakeArgString(Path));
    return true;
  };

  // Silence warnings for flags consumed only by the compiler.
  Args.ClaimAllArgs(options::OPT_g_Group);
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  Args.ClaimAllArgs(options::OPT_w);
  Args.ClaimAllArgs(options::OPT_stdlib_EQ);

  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      if (llvm::sys::path::filename(Input.getFilename()).equals_insensitive(
              "c.lib")) {
        HasExplicitLibC = true;
        break;
      }
      continue;
    }

    const Arg &A = Input.getInputArg();
    if (!A.getOption().matches(options::OPT_l))
      continue;

    StringRef Lib = A.getValue();
    if (Lib.equals_insensitive("c") || Lib.equals_insensitive("c.lib")) {
      HasExplicitLibC = true;
      break;
    }
  }

  // Enforce lld-link.
  StringRef Req = Args.getLastArgValue(options::OPT_fuse_ld_EQ);
  if (!Req.empty()) {
    if (Req.equals_insensitive("lld"))
      Req = "lld-link";
    if (!Req.equals_insensitive("lld-link")) {
      D.Diag(diag::err_drv_unsupported_opt)
          << Args.MakeArgString(Twine("-fuse-ld=") + Req +
                                " (NT-POSIX requires lld-link)");
      return;
    }
  }

  // Output.
  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(std::string("-out:") + Output.getFilename()));

  // Machine.
  if (TC.getArch() == llvm::Triple::x86)
    CmdArgs.push_back("-machine:x86");
  else if (TC.getArch() == llvm::Triple::aarch64)
    CmdArgs.push_back("-machine:arm64");
  else
    CmdArgs.push_back("-machine:x64");

  // Subsystem.
  bool isDLL = Args.hasArg(options::OPT__SLASH_LD, options::OPT__SLASH_LDd,
                           options::OPT_shared);
  if (!isDLL) {
    Arg *SubsysArg =
        Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
    if (SubsysArg && SubsysArg->getOption().matches(options::OPT_mwindows))
      CmdArgs.push_back("-subsystem:windows");
    else
      CmdArgs.push_back("-subsystem:console");
  }

  if (isDLL) {
    CmdArgs.push_back("-dll");

    SmallString<128> ImplibName(Output.getFilename());
    llvm::sys::path::replace_extension(ImplibName, "lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImplibName));

    StringRef EntryPoint;
    switch (TC.getArch()) {
    default:
      llvm_unreachable("unsupported architecture");
    case llvm::Triple::aarch64:
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
    case llvm::Triple::x86_64:
      EntryPoint = "_DllMainCRTStartup";
      break;
    case llvm::Triple::x86:
      EntryPoint = "_DllMainCRTStartup@12";
      break;
    }
    CmdArgs.push_back(Args.MakeArgString("-entry:" + EntryPoint));
  } else if (LinkStartFiles) {
    CmdArgs.push_back("-entry:mainCRTStartup");
  }

  // User -L paths.
  if (Args.hasArg(options::OPT_L))
    for (const auto &LibPath : Args.getAllArgValues(options::OPT_L))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));

  // Driver-owned runtime libraries live under the toolchain install root.
  TC.AddRuntimeLibSearchPaths(Args, CmdArgs);

  CmdArgs.push_back("-nologo");

  // CRT startup objects.
  //
  // crt_tls.obj provides every Windows PE image's TLS infrastructure
  // (_tls_index, IMAGE_TLS_DIRECTORY, .CRT$XLAA cookie init, .CRT$XLB dyn
  // TLS init) and is always linked.
  //
  // crt_tls_cleanup.obj is the .CRT$XLC slot that registers the libc
  // thread-detach cleanup callback. It belongs in the consumer EXE only —
  // c.dll already runs the cleanup itself and a duplicate registration
  // would make LdrShutdownThread fire it twice per external thread exit.
  // User DLLs also skip it so the EXE's slot remains the single source of
  // truth.
  if (LinkStartFiles) {
    if (!AddRequiredFile(isDLL ? "dllcrt.obj" : "crt1.obj") ||
        (!isDLL && !AddRequiredFile("crt_do_start.obj")) ||
        !AddRequiredFile("crt_tls.obj") ||
        (!isDLL && !AddRequiredFile("crt_tls_cleanup.obj")) ||
        !AddRequiredFile("crt_gs.obj") ||
        !AddRequiredFile("crt_cfg.obj") ||
        !AddRequiredFile("crt_loadcfg.obj"))
      return;
  }

  if (LinkDefaultLibs) {
    // C++ standard library.
    if (TC.ShouldLinkCXXStdlib(Args))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);

    // Unwinder.
    ToolChain::UnwindLibType UNW = TC.GetUnwindLibType(Args);
    if (UNW == ToolChain::UNW_CompilerRT)
      CmdArgs.push_back("unwind.lib");

    // compiler-rt builtins.
    CmdArgs.push_back(TC.getCompilerRTArgString(Args, "builtins"));

    // llvm-libc and NT kernel libraries. Under -static-libc we pull the
    // static archive (libc.lib) and suppress c.dll's import library so the
    // EXE is self-contained; otherwise we link the import library and rely
    // on c.dll at runtime. The mode is coherent with the -D_LIBC_DLL
    // injection in addClangTargetOptions: LIBC_API / __LIBC_DATA_IMPORT /
    // __LIBC_FUNC_IMPORT all expand correctly for the chosen path.
    if (LinkLibC) {
      const bool StaticLibC = Args.hasArg(options::OPT_static_libc);
      if (StaticLibC) {
        if (!AddRequiredFile("libc.lib"))
          return;
        CmdArgs.push_back("-nodefaultlib:c");
        CmdArgs.push_back("-nodefaultlib:c.lib");
      } else {
        if (!AddRequiredFile("c.lib"))
          return;
      }
    }
  }

  // Auto-import enables .refptr. stub collapsing in lld for cross-DLL
  // variable references. No runtime pseudo-reloc table is needed —
  // RTTI data refs use dynamic init + SEC_NO_CHANGE sealing instead.
  CmdArgs.push_back("-auto-import");
  CmdArgs.push_back("-llditanium");

  // Block all MSVC CRT libraries — NT-POSIX never uses them.
  if (!NoDefaultLibs) {
    CmdArgs.push_back("-nodefaultlib:msvcrt");
    CmdArgs.push_back("-nodefaultlib:msvcrtd");
    CmdArgs.push_back("-nodefaultlib:vcruntime");
    CmdArgs.push_back("-nodefaultlib:vcruntimed");
    CmdArgs.push_back("-nodefaultlib:ucrt");
    CmdArgs.push_back("-nodefaultlib:ucrtd");
    CmdArgs.push_back("-nodefaultlib:libcmt");
    CmdArgs.push_back("-nodefaultlib:libcmtd");
    CmdArgs.push_back("-nodefaultlib:oldnames");
  }

  // Sanitizer support.
  if (TC.getSanitizerArgs(Args).needsFuzzer()) {
    if (!Args.hasArg(options::OPT_shared))
      CmdArgs.push_back(Args.MakeArgString(
          Twine("-wholearchive:") + TC.getCompilerRTArgString(Args, "fuzzer")));
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
  }

  if (TC.getSanitizerArgs(Args).needsAsanRt()) {
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
    CmdArgs.push_back(TC.getCompilerRTArgString(Args, "asan_dynamic"));
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-wholearchive:") +
        TC.getCompilerRT(Args, "asan_dynamic_runtime_thunk")));
    CmdArgs.push_back(Args.MakeArgString(
        TC.getArch() == llvm::Triple::x86 ? "-include:___asan_seh_interceptor"
                                          : "-include:__asan_seh_interceptor"));
  }

  // LTO options.
  if (D.isUsingLTO()) {
    if (Arg *A = tools::getLastProfileSampleUseArg(Args))
      CmdArgs.push_back(
          Args.MakeArgString(Twine("-lto-sample-profile:") + A->getValue()));

    if (Args.hasFlag(options::OPT_gsplit_dwarf, options::OPT_gno_split_dwarf,
                     false))
      CmdArgs.push_back(Args.MakeArgString(Twine("-dwodir:") +
                                           Output.getFilename() + "_dwo"));
  }

  // /Brepro
  if (!Args.hasFlag(options::OPT_mincremental_linker_compatible,
                    options::OPT_mno_incremental_linker_compatible, true))
    CmdArgs.push_back("-Brepro");

  if (Args.hasArg(options::OPT_fms_hotpatch, options::OPT__SLASH_hotpatch))
    CmdArgs.push_back("-functionpadmin");

  // Control Flow Guard.
  for (const Arg *A :
       Args.filtered(options::OPT__SLASH_guard, options::OPT_mguard_EQ)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs.equals_insensitive("cf") ||
        GuardArgs.equals_insensitive("cf,nochecks"))
      CmdArgs.push_back("-guard:cf");
    else if (GuardArgs.equals_insensitive("cf-"))
      CmdArgs.push_back("-guard:cf-");
    else if (GuardArgs.equals_insensitive("ehcont"))
      CmdArgs.push_back("-guard:ehcont");
    else if (GuardArgs.equals_insensitive("ehcont-"))
      CmdArgs.push_back("-guard:ehcont-");
    else
      D.Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << GuardArgs;
  }

  if (Args.hasArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    CmdArgs.push_back("-debug");

  // Forward explicit /link args.
  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);

  // Inputs.
  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }
    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l)) {
      StringRef Lib = A.getValue();
      CmdArgs.push_back(
          Args.MakeArgString(Lib.ends_with(".lib") ? Lib : (Lib + ".lib")));
      continue;
    }
    A.renderAsInput(Args, CmdArgs);
  }

  // llvm-libc is layered directly on NT imports. Keep those base platform
  // dependencies after libc in the final link line so explicit "-lc" probes
  // under -nodefaultlibs behave like ordinary library checks.
  if (LinkLibC || HasExplicitLibC) {
    if (!AddRequiredFile("ntdll.lib") ||
        !AddRequiredFile("sspicli.lib") ||
        !AddRequiredFile("bcryptprimitives.lib"))
      return;
  }

  // Offload/profile.
  TC.addOffloadRTLibs(C.getActiveOffloadKinds(), Args, CmdArgs);
  TC.addProfileRTLibs(Args, CmdArgs);
  TC.NormalizeLLDLinkArgs(Args, CmdArgs);

  // Linker path.
  const char *LinkerExe = Args.MakeArgString(TC.GetProgramPath("lld-link"));

  auto LinkCmd = std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(), LinkerExe, CmdArgs,
      Inputs, Output);

  C.addCommand(std::move(LinkCmd));
}
