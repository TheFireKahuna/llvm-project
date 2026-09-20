//===--- WindowsItanium.cpp - Windows Itanium ToolChain -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItanium.h"
#include "Clang.h"
#include "MSVC.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

using llvm::VersionTuple;

DerivedArgList *
WindowsItaniumToolChain::TranslateArgs(const DerivedArgList &Args,
                                       StringRef BoundArch,
                                       Action::OffloadKind OFK) const {
  DerivedArgList *DAL = translateMSVCCompatibleArgs(*this, Args, OFK);
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

static bool canExecute(llvm::vfs::FileSystem &VFS, llvm::StringRef Path) {
  auto Status = VFS.status(Path);
  if (!Status)
    return false;
  return (Status->getPermissions() & llvm::sys::fs::perms::all_exe) != 0;
}


static std::string FindLLVMExecutable(const ToolChain &TC, const char *Exe) {
  llvm::SmallString<128> P(TC.GetProgramPath(Exe));
  // GetProgramPath already tries driver-relative and PATH search;
  // we still sanity check if the resolved path is executable.
  if (!P.empty() && canExecute(TC.getVFS(), P))
    return std::string(P.str());
  return std::string(Exe);
}
void windowsitanium::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                         const InputInfo &Output,
                                         const InputInfoList &Inputs,
                                         const ArgList &Args,
                                         const char *LinkingOutput) const {
  ArgStringList CmdArgs;

  auto &TC = static_cast<const WindowsItaniumToolChain &>(getToolChain());
  const Driver &D = C.getDriver();
  const bool NoStdLib = Args.hasArg(options::OPT_nostdlib);
  const bool NoStartFiles = Args.hasArg(options::OPT_nostartfiles);
  const bool NoDefaultLibs = Args.hasArg(options::OPT_nodefaultlibs);
  const bool NoLibC = Args.hasArg(options::OPT_nolibc);
  const bool LinkStartFiles = !NoStdLib && !NoStartFiles;
  const bool LinkDefaultLibs = !NoStdLib && !NoDefaultLibs;
  const bool LinkLibC = LinkDefaultLibs && !NoLibC;

  // Silence warning for "clang -g foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_g_Group);
  // and "clang -emit-llvm foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  // and for "clang -w foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_w);
  // and for "-stdlib=libc++" during linking
  Args.ClaimAllArgs(options::OPT_stdlib_EQ);

  // We only support lld-link as the linker for this toolchain.
  // -fuse-ld=lld is accepted and normalized to lld-link.
  StringRef Req = Args.getLastArgValue(options::OPT_fuse_ld_EQ);
  if (!Req.empty()) {
    if (Req.equals_insensitive("lld"))
      Req = "lld-link";
    if (!Req.equals_insensitive("lld-link")) {
      C.getDriver().Diag(diag::err_drv_unsupported_opt)
          << Args.MakeArgString(Twine("-fuse-ld=") + Req +
                                " (Windows-Itanium requires lld-link)");
      return;
    }
  }

  // Output
  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(std::string("-out:") + Output.getFilename()));

  // Machine / target
  // (Adjust if your Itanium triple maps differently; typically x64)
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

    // x86 uses @12 decoration for stdcall parameters.
    StringRef entryPoint;
    switch (TC.getArch()) {
    default:
      llvm_unreachable("unsupported architecture");
    case llvm::Triple::aarch64:
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
    case llvm::Triple::x86_64:
      entryPoint = "_DllMainCRTStartup";
      break;
    case llvm::Triple::x86:
      entryPoint = "_DllMainCRTStartup@12";
      break;
    }
    CmdArgs.push_back(Args.MakeArgString("-entry:" + entryPoint));
  } else {
    if (LinkStartFiles) {
      Arg *SubsysArg =
          Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
      if (SubsysArg && SubsysArg->getOption().matches(options::OPT_mwindows))
        CmdArgs.push_back("-entry:WinMainCRTStartup");
      else
        CmdArgs.push_back("-entry:mainCRTStartup");
    }
  }

  if (const Arg *A = Args.getLastArg(options::OPT_fveclib)) {
    StringRef V = A->getValue();
    if (V == "ArmPL")
      CmdArgs.push_back(Args.MakeArgString("--dependent-lib=amath"));
  }

  // Add UCRT + Windows SDK lib paths.
  if (!NoStdLib) {
    if (TC.useUniversalCRT()) {
      std::string UCRTLib;
      if (TC.getUniversalCRTLibraryPath(Args, UCRTLib))
        CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + UCRTLib));
    }

    std::string WinSDKLib;
    if (TC.getWindowsSDKLibraryPath(Args, WinSDKLib))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + WinSDKLib));
  }

  // User -L
  if (Args.hasArg(options::OPT_L))
    for (const auto &LibPath : Args.getAllArgValues(options::OPT_L))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));

  // Driver-owned runtime libraries live under the toolchain install root.
  TC.AddRuntimeLibSearchPaths(Args, CmdArgs);
  CmdArgs.push_back("-nologo");

  // The runtime libraries are default libraries: lld searches them after
  // every positional input, so a definition in the user's objects or
  // libraries takes precedence over the same symbol in a runtime archive,
  // as it does with link.exe and the MSVC driver.
  if (LinkDefaultLibs) {
    // C++ standard library. clang-cl has no C-only mode, so it links the
    // library whenever the inputs may be C++; lld pulls members only when
    // they are referenced, so a C program gains no dependency on c++.dll.
    if (TC.ShouldLinkCXXStdlib(Args) ||
        (D.IsCLMode() && !Args.hasArg(options::OPT_nostdlibxx)))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);

    // Unwinder - SEH-based libunwind for both runtime modes.
    ToolChain::UnwindLibType UNW = TC.GetUnwindLibType(Args);
    if (UNW != ToolChain::UNW_None && UNW != ToolChain::UNW_CompilerRT) {
      if (const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ)) {
        TC.getDriver().Diag(diag::err_drv_unsupported_unwind_for_platform)
            << A->getValue() << TC.getTriple().normalize();
      }
    } else if (UNW == ToolChain::UNW_CompilerRT) {
      CmdArgs.push_back("-defaultlib:unwind.lib");
    }

    CmdArgs.push_back(Args.MakeArgString(
        Twine("-defaultlib:") + TC.getCompilerRTArgString(Args, "builtins")));

    if (LinkLibC) {
      CmdArgs.push_back("-defaultlib:ucrt.lib");
      CmdArgs.push_back("-defaultlib:kernel32.lib");

      // wincrt provides CRT startup, __cxa_atexit, security cookie, etc.
      std::string WinCRT = TC.getCompilerRT(Args, "wincrt");
      if (TC.getVFS().exists(WinCRT))
        CmdArgs.push_back(Args.MakeArgString(
            Twine("-defaultlib:") + TC.getCompilerRTBasename(Args, "wincrt")));
      // wincrt's startup/security/loadconfig code calls ntdll directly
      // (NtProtectVirtualMemory, RtlAllocateHeap, RtlImageNtHeader, ...),
      // so ntdll.lib is required whenever wincrt is linked.
      CmdArgs.push_back("-defaultlib:ntdll.lib");

      // memcpy/memset/memmove/memcmp/memchr and the SEH personality live in
      // ucrtbase.dll but are absent from ucrt.lib (Microsoft supplies them
      // via vcruntime.lib, which this target never links). This generated
      // import lib -- shipped alongside wincrt in the resource dir --
      // resolves them for ordinary C/C++ codegen. Referenced by basename so
      // lld-link finds it on the compiler-rt search path added above.
      std::string UcrtMem = TC.getCompilerRT(Args, "ucrt_memory");
      if (TC.getVFS().exists(UcrtMem))
        CmdArgs.push_back(
            Args.MakeArgString(Twine("-defaultlib:") +
                               TC.getCompilerRTBasename(Args, "ucrt_memory")));
    }
  }

  // Static data that holds the address of a symbol from another DLL is
  // filled by the loader through an import descriptor of its own; wincrt
  // applies the addends the loader cannot. A definition in the link takes
  // precedence over an import library's entry for the same name.
  CmdArgs.push_back("-import-slots");

  // A delay-loaded import is called through a table the loader writes, which
  // no indirect-call check covers. Giving that table a section of its own
  // lets the loader keep it read-only except while it resolves an import.
  CmdArgs.push_back("-delayload-protect");

  // Block the MSVC CRT libraries that objects may request through
  // /DEFAULTLIB directives.
  if (!NoDefaultLibs) {
    CmdArgs.push_back("-nodefaultlib:msvcrt");
    CmdArgs.push_back("-nodefaultlib:msvcrtd");
    CmdArgs.push_back("-nodefaultlib:vcruntime");
    CmdArgs.push_back("-nodefaultlib:vcruntimed");
    CmdArgs.push_back("-nodefaultlib:libcmt");
    CmdArgs.push_back("-nodefaultlib:libcmtd");
    CmdArgs.push_back("-nodefaultlib:oldnames");
    CmdArgs.push_back("-nodefaultlib:ucrtd");
    // Objects carry /DEFAULTLIB:iso_stdio_wide_specifiers.lib (injected by
    // UCRT headers under the driver-defined _CRT_STDIO_ISO_WIDE_SPECIFIERS),
    // but that library lives in the VC-Tools lib directory this path never
    // searches. wincrt satisfies the accompanying forced-include marker
    // symbol; suppress the library lookup itself.
    CmdArgs.push_back("-nodefaultlib:iso_stdio_wide_specifiers");

    if (LinkLibC) {
      CmdArgs.push_back("-defaultlib:user32");
      CmdArgs.push_back("-defaultlib:advapi32");
      CmdArgs.push_back("-defaultlib:shell32");
    }
  }

  if (TC.getSanitizerArgs(Args).needsFuzzer()) {
    if (!Args.hasArg(options::OPT_shared))
      CmdArgs.push_back(Args.MakeArgString(
          Twine("-wholearchive:") + TC.getCompilerRTArgString(Args, "fuzzer")));
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
  }

  // ASan uses dynamic runtime since Windows Itanium uses dynamic ucrt.
  if (TC.getSanitizerArgs(Args).needsAsanRt()) {
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
    CmdArgs.push_back(TC.getCompilerRTArgString(Args, "asan_dynamic"));
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-wholearchive:") +
        TC.getCompilerRT(Args, "asan_dynamic_runtime_thunk")));
    // Prevent ASan SEH interceptor from being optimized out.
    CmdArgs.push_back(Args.MakeArgString(
        TC.getArch() == llvm::Triple::x86 ? "-include:___asan_seh_interceptor"
                                          : "-include:__asan_seh_interceptor"));
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

  // /Brepro maps to -mno-incremental-linker-compatible.
  if (!Args.hasFlag(options::OPT_mincremental_linker_compatible,
                    options::OPT_mno_incremental_linker_compatible,
                    /*Default=*/true))
    CmdArgs.push_back("-Brepro");

  if (Args.hasArg(options::OPT_fms_hotpatch, options::OPT__SLASH_hotpatch))
    CmdArgs.push_back("-functionpadmin");

  TC.addGuardLinkArgs(Args, CmdArgs, isDLL);

  if (Args.hasArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    CmdArgs.push_back("-debug");

  // Forward explicit /link args.
  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);

  // Inputs
  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }

    const Arg &A = Input.getInputArg();

    // Render -l => foo.lib for link-like drivers
    if (A.getOption().matches(options::OPT_l)) {
      StringRef Lib = A.getValue();
      CmdArgs.push_back(Args.MakeArgString(Lib.ends_with(".lib") ? Lib
                                                                : (Lib + ".lib")));
      continue;
    }

    A.renderAsInput(Args, CmdArgs);
  }

  // Offload/profile libs as appropriate (LLVM-side)
  TC.addOffloadRTLibs(C.getActiveOffloadKinds(), Args, CmdArgs);
  TC.addProfileRTLibs(Args, CmdArgs);
  TC.NormalizeLLDLinkArgs(Args, CmdArgs);

  // The library search path is entirely driver-owned: a Visual Studio
  // developer shell's LIB and VC installation variables must not reach
  // lld-link, or MSVC's runtime libraries would be found.
  CmdArgs.push_back("-lldignoreenv");

  // Choose linker path: ALWAYS lld-link (LLVM-first).
  llvm::SmallString<128> LinkPath(FindLLVMExecutable(TC, "lld-link.exe"));

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(LinkPath), CmdArgs, Inputs, Output));
}

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

  // Define __MSVCRT__ so headers and libraries know the C runtime provides
  // underscore-prefixed functions (_access, _open, _vsnprintf, etc.).
  // Mirrors MinGW's convention.
  CC1Args.push_back("-D__MSVCRT__");

  // The UCRT is always linked dynamically. _DLL makes its headers declare its
  // functions and data imported, as they are under MSVC's /MD.
  CC1Args.push_back("-D_DLL");

  // Clang's resource headers (intrin.h, xmmintrin.h, yvals_core.h, ...) gate
  // their MSVC-intrinsic emulation on LLVM_CRT_UCRT rather than _MSC_VER,
  // which this target deliberately does not define. Inside the LLVM build the
  // macro comes from llvm-config.h; every other consumer of UCRT headers
  // (runtimes, user code) needs the driver to provide it, otherwise intrin.h
  // falls through to the raw MSVC header and clashes with clang's builtins.
  CC1Args.push_back("-DLLVM_CRT_UCRT");

  // ISO-conforming wide specifiers for wprintf/wscanf (%s = char*, %ls = wchar_t*).
  // Auto-links iso_stdio_wide_specifiers.lib via #pragma comment(lib, ...).
  CC1Args.push_back("-D_CRT_STDIO_ISO_WIDE_SPECIFIERS");

  // Suppress MSVC "insecure function" deprecation warnings for standard C.
  CC1Args.push_back("-D_CRT_SECURE_NO_WARNINGS");

  // Signals its safe to export UCRT functions through C++ modules
  CC1Args.push_back("-D_STATIC_INLINE_UCRT_FUNCTIONS=0");

  // Windows lacks sys/time.h.
  CC1Args.push_back("-UCLOCK_REALTIME");

  // Linker-only options; claim to suppress unused warnings.
  // -mthreads is MinGW's; the UCRT is always thread-safe.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole,
                   options::OPT_mthreads}) {
    if (Arg *A = DriverArgs.getLastArgNoClaim(Opt))
      A->ignoreTargetSpecific();
  }
  if (Arg *A = DriverArgs.getLastArgNoClaim(options::OPT_marm64x))
    A->ignoreTargetSpecific();
}

void WindowsItaniumToolChain::AddClangSystemIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  // Clang resource headers
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, getDriver().ResourceDir,
                                  "include", "", "");
  }

  // Explicit /imsvc paths are user-specified. You may choose to hard-reject these
  // if you want "no MSVC includes ever", even explicitly.
  for (const auto &Path : DriverArgs.getAllArgValues(options::OPT__SLASH_imsvc)) {
    // If you want to forbid it, diagnose and return instead:
    // getDriver().Diag(diag::err_drv_unsupported_opt) << "/imsvc (forbidden)";
    addSystemInclude(DriverArgs, CC1Args, Path);
  }

  // /external:env:... is also explicit, but you may want to restrict it.
  auto AddSystemIncludesFromEnv = [&](StringRef Var) -> bool {
    if (auto Val = llvm::sys::Process::GetEnv(Var)) {
      SmallVector<StringRef, 8> Dirs;
      StringRef(*Val).split(Dirs, ";", /*MaxSplit=*/-1, /*KeepEmpty=*/false);
      if (!Dirs.empty()) {
        addSystemIncludes(DriverArgs, CC1Args, Dirs);
        return true;
      }
    }
    return false;
  };

  for (const auto &Var : DriverArgs.getAllArgValues(options::OPT__SLASH_external_env))
    AddSystemIncludesFromEnv(Var);

  // DO NOT honor INCLUDE/EXTERNAL_INCLUDE implicitly.
  // This prevents vcvars* from injecting VC include paths.
  // (If you want to allow *explicit* env-based injection, use /external:env:. above.)

  // -nostdlibinc suppresses C library headers (UCRT) but not
  // Windows SDK headers (shared/, um/).
  if (!DriverArgs.hasArg(options::OPT_nostdlibinc)) {
    if (useUniversalCRT()) {
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
  }

  // Windows SDK includes (shared/um/winrt/cppwinrt).
  std::string WindowsSDKDir;
  int Major = 0;
  std::string IncludeVer;
  std::string LibVer;
  if (llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                             WindowsSDKDir, Major, IncludeVer, LibVer)) {
    if (Major >= 10) {
      if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) && WinSdkVersion.has_value())
        IncludeVer = LibVer = *WinSdkVersion;
    }

    if (Major >= 8) {
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", IncludeVer, "shared");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", IncludeVer, "um");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", IncludeVer, "winrt");

      if (Major >= 10) {
        llvm::VersionTuple Tuple;
        if (!Tuple.tryParse(IncludeVer) && Tuple.getSubminor().value_or(0) >= 17134) {
          AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                        "Include", IncludeVer, "cppwinrt");
        }
      }
    } else {
      // Pre-8 SDK fallback (optional). You can also hard-fail instead.
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", "", "");
    }
  }

  // NO fallback guessing of VC install paths.

  // NO Visual Studio / VC-Tools include directory. This target's C/C++ header
  // world is UCRT + Windows SDK + clang's own resource headers (which provide
  // self-contained vcruntime.h / vadefs.h / intrin.h). Adding the VC-Tools
  // include here would pull real MSVC headers, defeating the wincrt design.
}

ToolChain::RuntimeLibType
WindowsItaniumToolChain::GetDefaultRuntimeLibType() const {
  return ToolChain::RLT_CompilerRT;
}

void WindowsItaniumToolChain::printVerboseInfo(raw_ostream &OS) const {
  WindowsItaniumBaseToolChain::printVerboseInfo(OS);
}

Tool *WindowsItaniumToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

bool WindowsItaniumToolChain::getWindowsSDKLibraryPath(const ArgList &Args,
                                                       std::string &Path) const {
  std::string WindowsSDKDir;
  int Major = 0;
  std::string IncludeVer;
  std::string LibVer;

  Path.clear();
  if (!llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                              WindowsSDKDir, Major, IncludeVer, LibVer))
    return false;

  // If user pinned version, use it consistently.
  if (Major >= 10) {
    if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) && WinSdkVersion.has_value())
      IncludeVer = LibVer = *WinSdkVersion;
  }

  StringRef ArchName = llvm::archToWindowsSDKArch(getArch());
  if (ArchName.empty())
    return false;

  llvm::SmallString<128> LibPath(WindowsSDKDir);

  // Windows SDK lib layout:
  //   <SDK>/Lib/<ver>/um/<arch>
  // plus ucrt handled separately.
  if (Major >= 10) {
    llvm::sys::path::append(LibPath, "Lib", LibVer, "um", ArchName);
  } else if (Major >= 8) {
    llvm::sys::path::append(LibPath, "Lib", "winv6.3", "um", ArchName);
  } else {
    // Old SDKs vary; you can either fail hard or provide a conservative append.
    return false;
  }

  Path = std::string(LibPath);
  return true;
}

bool WindowsItaniumToolChain::useUniversalCRT() const {
  std::string SdkPath, Ver;
  return llvm::getUniversalCRTSdkDir(getVFS(), WinSdkDir, WinSdkVersion,
                                     WinSysRoot, SdkPath, Ver);
}


bool WindowsItaniumToolChain::getUniversalCRTLibraryPath(const ArgList &Args,
                                                         std::string &Path) const {
  std::string UniversalCRTSdkPath;
  std::string UCRTVersion;

  Path.clear();
  if (!llvm::getUniversalCRTSdkDir(getVFS(), WinSdkDir, WinSdkVersion,
                                   WinSysRoot, UniversalCRTSdkPath,
                                   UCRTVersion))
    return false;

  // If only /winsdkversion was given, allow it to pin UCRT version too.
  if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) && WinSdkVersion.has_value())
    UCRTVersion = *WinSdkVersion;

  StringRef ArchName = llvm::archToWindowsSDKArch(getArch());
  if (ArchName.empty())
    return false;

  llvm::SmallString<128> LibPath(UniversalCRTSdkPath);
  llvm::sys::path::append(LibPath, "Lib", UCRTVersion, "ucrt", ArchName);

  Path = std::string(LibPath);
  return true;
}
