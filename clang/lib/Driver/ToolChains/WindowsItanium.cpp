//===--- WindowsItanium.cpp - Windows Itanium ToolChain -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "WindowsItanium.h"
#include "Clang.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Config/config.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

#if defined(LLVM_RUNTIME_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOGDI
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

using llvm::VersionTuple;
using LibCMode = WindowsItaniumToolChain::LibCMode;

// Translate MSVC-style /O flags to clang equivalents.
static void TranslateOptArg(Arg *A, DerivedArgList &DAL,
                            bool SupportsForcingFramePointer,
                            const char *ExpandChar, const OptTable &Opts) {
  assert(A->getOption().matches(options::OPT__SLASH_O));

  StringRef OptStr = A->getValue();
  for (size_t I = 0, E = OptStr.size(); I != E; ++I) {
    const char &OptChar = *(OptStr.data() + I);
    switch (OptChar) {
    default:
      break;
    case '1':
    case '2':
    case 'x':
    case 'd':
      // Ignore /O[12xd] flags that aren't the last one on the command line.
      // Only the last one gets expanded.
      if (&OptChar != ExpandChar) {
        A->claim();
        break;
      }
      if (OptChar == 'd') {
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_O0));
      } else {
        if (OptChar == '1') {
          DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "s");
        } else if (OptChar == '2' || OptChar == 'x') {
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fbuiltin));
          DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "3");
        }
        if (SupportsForcingFramePointer &&
            !DAL.hasArgNoClaim(options::OPT_fno_omit_frame_pointer))
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fomit_frame_pointer));
        if (OptChar == '1' || OptChar == '2')
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_ffunction_sections));
      }
      break;
    case 'b':
      if (I + 1 != E && isdigit(OptStr[I + 1])) {
        switch (OptStr[I + 1]) {
        case '0':
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_inline));
          break;
        case '1':
          DAL.AddFlagArg(A,
                         Opts.getOption(options::OPT_finline_hint_functions));
          break;
        case '2':
        case '3':
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_finline_functions));
          break;
        }
        ++I;
      }
      break;
    case 'g':
      A->claim();
      break;
    case 'i':
      if (I + 1 != E && OptStr[I + 1] == '-') {
        ++I;
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_builtin));
      } else {
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fbuiltin));
      }
      break;
    case 's':
      DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "s");
      break;
    case 't':
      DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "3");
      break;
    case 'y': {
      bool OmitFramePointer = true;
      if (I + 1 != E && OptStr[I + 1] == '-') {
        OmitFramePointer = false;
        ++I;
      }
      if (SupportsForcingFramePointer) {
        if (OmitFramePointer)
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fomit_frame_pointer));
        else
          DAL.AddFlagArg(A,
                         Opts.getOption(options::OPT_fno_omit_frame_pointer));
      } else {
        // /Oy- has no effect on x86-64; claim to suppress warning.
        A->claim();
      }
      break;
    }
    }
  }
}

// Translate -Dfoo#bar into -Dfoo=bar (MSVC-style macro definition).
static void TranslateDArg(Arg *A, DerivedArgList &DAL, const OptTable &Opts) {
  assert(A->getOption().matches(options::OPT_D));

  StringRef Val = A->getValue();
  size_t Hash = Val.find('#');
  if (Hash == StringRef::npos || Hash > Val.find('=')) {
    DAL.append(A);
    return;
  }

  std::string NewVal = std::string(Val);
  NewVal[Hash] = '=';
  DAL.AddJoinedArg(A, Opts.getOption(options::OPT_D), NewVal);
}

// Translate /permissive to disable two-phase lookup and operator names.
static void TranslatePermissive(Arg *A, DerivedArgList &DAL,
                                const OptTable &Opts) {
  DAL.AddFlagArg(A, Opts.getOption(options::OPT__SLASH_Zc_twoPhase_));
  DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_operator_names));
}

// Translate /permissive- to enable two-phase lookup and operator names.
static void TranslatePermissiveMinus(Arg *A, DerivedArgList &DAL,
                                     const OptTable &Opts) {
  DAL.AddFlagArg(A, Opts.getOption(options::OPT__SLASH_Zc_twoPhase));
  DAL.AddFlagArg(A, Opts.getOption(options::OPT_foperator_names));
}

DerivedArgList *
WindowsItaniumToolChain::TranslateArgs(const DerivedArgList &Args,
                                       StringRef BoundArch,
                                       Action::OffloadKind OFK) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  const OptTable &Opts = getDriver().getOpts();

  // /Oy and /Oy- don't have an effect on X86-64.
  bool SupportsForcingFramePointer = getArch() != llvm::Triple::x86_64;

  // The -O[12xd] flag actually expands to several flags. We must desugar the
  // flags so that options embedded can be negated. For example, the '-O2' flag
  // enables '-Oy'. Expanding '-O2' into its constituent flags allows us to
  // correctly handle '-O2 -Oy-' where the trailing '-Oy-' disables a single
  // aspect of '-O2'.
  //
  // Note that this expansion logic only applies to the *last* of '[12xd]'.

  // First step is to search for the character we'd like to expand.
  const char *ExpandChar = nullptr;
  for (Arg *A : Args.filtered(options::OPT__SLASH_O)) {
    StringRef OptStr = A->getValue();
    for (size_t I = 0, E = OptStr.size(); I != E; ++I) {
      char OptChar = OptStr[I];
      char PrevChar = I > 0 ? OptStr[I - 1] : '0';
      if (PrevChar == 'b') {
        // OptChar does not expand; it's an argument to the previous char.
        continue;
      }
      if (OptChar == '1' || OptChar == '2' || OptChar == 'x' || OptChar == 'd')
        ExpandChar = OptStr.data() + I;
    }
  }

  for (Arg *A : Args) {
    if (A->getOption().matches(options::OPT__SLASH_O)) {
      // The -O flag actually takes an amalgam of other options. For example,
      // '/Ogyb2' is equivalent to '/Og' '/Oy' '/Ob2'.
      TranslateOptArg(A, *DAL, SupportsForcingFramePointer, ExpandChar, Opts);
    } else if (A->getOption().matches(options::OPT_D)) {
      // Translate -Dfoo#bar into -Dfoo=bar.
      TranslateDArg(A, *DAL, Opts);
    } else if (A->getOption().matches(options::OPT__SLASH_permissive)) {
      // Expand /permissive
      TranslatePermissive(A, *DAL, Opts);
    } else if (A->getOption().matches(options::OPT__SLASH_permissive_)) {
      // Expand /permissive-
      TranslatePermissiveMinus(A, *DAL, Opts);
    } else if (A->getOption().matches(options::OPT_fdwarf_exceptions) ||
               A->getOption().matches(options::OPT_fwasm_exceptions)) {
      // Only SEH and SJLJ exceptions are supported.
      getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
          << A->getAsString(Args) << getTriple().str();
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
    } else if (A->getOption().matches(options::OPT_mthreads)) {
      // MinGW-specific; MSVC runtime is already thread-safe.
      A->ignoreTargetSpecific();
    } else if (OFK != Action::OFK_HIP) {
      // HIP Toolchain translates input args by itself.
      DAL->append(A);
    }
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
  const bool UseLLVMLibC = TC.usesLLVMLibC();
  const bool NoStdLib = Args.hasArg(options::OPT_nostdlib);
  const bool NoStartFiles = Args.hasArg(options::OPT_nostartfiles);
  const bool NoDefaultLibs = Args.hasArg(options::OPT_nodefaultlibs);
  const bool NoLibC = Args.hasArg(options::OPT_nolibc);
  const bool LinkStartFiles = !NoStdLib && !NoStartFiles;
  const bool LinkDefaultLibs = !NoStdLib && !NoDefaultLibs;
  const bool LinkLibC = LinkDefaultLibs && !NoLibC;

  auto AddRequiredInstalledFile = [&](const char *Name) -> bool {
    std::string Path = TC.GetFilePath(Name);
    if (!TC.getVFS().exists(Path)) {
      D.Diag(diag::err_drv_no_such_file) << Name;
      return false;
    }
    CmdArgs.push_back(Args.MakeArgString(Path));
    return true;
  };

  auto NeedsDelayLoadRuntime = [&]() {
    auto IsDelayLoadArg = [](StringRef Value) {
      return Value.starts_with_insensitive("/delayload:") ||
             Value.starts_with_insensitive("-delayload:");
    };

    for (const Arg *A :
         Args.filtered(options::OPT__SLASH_link, options::OPT_Xlinker,
                       options::OPT_Wl_COMMA)) {
      for (const char *Value : A->getValues()) {
        if (IsDelayLoadArg(Value))
          return true;
      }
    }
    return false;
  };

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

  // Add UCRT + Windows SDK lib paths (only).
  if (!NoStdLib) {
    if (!UseLLVMLibC && TC.useUniversalCRT()) {
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

  // LLVM runtimes (compiler-rt, libc++ libs, unwind, etc.) if present.
  // These are "LLVM paths", not MSVC.
  for (const auto &LibPath : TC.getLibraryPaths()) {
    if (TC.getVFS().exists(LibPath))
      CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + LibPath));
  }
  auto CRTPath = TC.getCompilerRTPath();
  if (TC.getVFS().exists(CRTPath))
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + CRTPath));
  CmdArgs.push_back("-nologo");

  // Runtime library selection.
  ToolChain::RuntimeLibType RLT = TC.GetRuntimeLibType(Args);

  if (UseLLVMLibC && LinkDefaultLibs && RLT != ToolChain::RLT_CompilerRT) {
    StringRef RuntimeName = "platform";
    if (const Arg *A = Args.getLastArg(options::OPT_rtlib_EQ))
      RuntimeName = A->getValue();
    D.Diag(diag::err_drv_unsupported_rtlib_for_platform)
        << RuntimeName << TC.getTriple().normalize();
    return;
  }

  if (LinkStartFiles && UseLLVMLibC) {
    if (!AddRequiredInstalledFile(isDLL ? "dllcrt.obj" : "crt1.obj") ||
        (!isDLL && !AddRequiredInstalledFile("crt_do_start.obj")) ||
        !AddRequiredInstalledFile("crt_tls.obj") ||
        !AddRequiredInstalledFile("crt_gs.obj") ||
        !AddRequiredInstalledFile("crt_cfg.obj") ||
        !AddRequiredInstalledFile("crt_loadcfg.obj"))
      return;
    if (NeedsDelayLoadRuntime() && !AddRequiredInstalledFile("crt_delayload.obj"))
      return;
  }

  if (LinkDefaultLibs) {
    // C++ standard library.
    if (TC.ShouldLinkCXXStdlib(Args))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);

    // Unwinder - SEH-based libunwind for both runtime modes.
    ToolChain::UnwindLibType UNW = TC.GetUnwindLibType(Args);
    if (UNW != ToolChain::UNW_None && UNW != ToolChain::UNW_CompilerRT) {
      if (const Arg *A = Args.getLastArg(options::OPT_unwindlib_EQ)) {
        TC.getDriver().Diag(diag::err_drv_unsupported_unwind_for_platform)
            << A->getValue() << TC.getTriple().normalize();
      }
    } else if (UNW == ToolChain::UNW_CompilerRT) {
      CmdArgs.push_back("unwind.lib");
    }

    if (RLT == ToolChain::RLT_CompilerRT) {
      // compiler-rt builtins remain the builtins provider in both libc modes.
      CmdArgs.push_back(TC.getCompilerRTArgString(Args, "builtins"));
    }

    if (UseLLVMLibC) {
      if (LinkLibC) {
        if (!AddRequiredInstalledFile("c.lib") ||
            !AddRequiredInstalledFile("kernelbase.lib") ||
            !AddRequiredInstalledFile("ntdll.lib") ||
            !AddRequiredInstalledFile("sspicli.lib") ||
            !AddRequiredInstalledFile("bcryptprimitives.lib"))
          return;

        CmdArgs.push_back("kernel32.lib");
        CmdArgs.push_back("bcrypt.lib");
      }
    } else if (LinkLibC) {
      CmdArgs.push_back("ucrt.lib");
      CmdArgs.push_back("kernel32.lib");

      if (RLT == ToolChain::RLT_CompilerRT) {
        // wincrt provides CRT startup, __cxa_atexit, security cookie, etc.
        std::string WinCRT = TC.getCompilerRT(Args, "wincrt");
        if (TC.getVFS().exists(WinCRT))
          CmdArgs.push_back(
              Args.MakeArgString(TC.getCompilerRTBasename(Args, "wincrt")));
      } else {
        CmdArgs.push_back("vcruntime.lib");
      }
    }
  }

  // Auto-import for vtable pseudo-relocations. LLD generates the table;
  // the CRT runtime patches it (_pei386_runtime_relocator).
  CmdArgs.push_back("-auto-import");
  CmdArgs.push_back("-runtime-pseudo-reloc");

  // Block conflicting MSVC CRT libraries.
  if (!NoDefaultLibs) {
    // Always block debug variants and static CRT.
    CmdArgs.push_back("-nodefaultlib:msvcrtd");
    CmdArgs.push_back("-nodefaultlib:vcruntimed");
    CmdArgs.push_back("-nodefaultlib:libcmt");
    CmdArgs.push_back("-nodefaultlib:libcmtd");
    CmdArgs.push_back("-nodefaultlib:oldnames");
    CmdArgs.push_back("-nodefaultlib:ucrtd");

    if (UseLLVMLibC || RLT == ToolChain::RLT_CompilerRT) {
      // Block system CRTs whenever llvm-libc owns the C runtime, and also
      // when compiler-rt is serving as the legacy system runtime personality.
      CmdArgs.push_back("-nodefaultlib:msvcrt");
      CmdArgs.push_back("-nodefaultlib:vcruntime");
    }
    if (UseLLVMLibC) {
      CmdArgs.push_back("-nodefaultlib:ucrt");
      CmdArgs.push_back("-nodefaultlib:wincrt");
    }

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

  // Control Flow Guard checks
  for (const Arg *A : Args.filtered(options::OPT__SLASH_guard, options::OPT_mguard_EQ)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs.equals_insensitive("cf") ||
        GuardArgs.equals_insensitive("cf,nochecks")) {
      // MSVC doesn't yet support the "nochecks" modifier.
      CmdArgs.push_back("-guard:cf");
    } else if (GuardArgs.equals_insensitive("cf-")) {
      CmdArgs.push_back("-guard:cf-");
    } else if (GuardArgs.equals_insensitive("ehcont")) {
      CmdArgs.push_back("-guard:ehcont");
    } else if (GuardArgs.equals_insensitive("ehcont-")) {
      CmdArgs.push_back("-guard:ehcont-");
    } else {
      D.Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << GuardArgs;
    }
  }

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

  // Choose linker path: ALWAYS lld-link (LLVM-first).
  llvm::SmallString<128> LinkPath(FindLLVMExecutable(TC, "lld-link.exe"));

  // Environment sanitization: prevent VC dev shells from contaminating tool lookup.
  // We intentionally *clear* LIB/INCLUDE so your toolchain logic is authoritative.
  std::vector<const char *> Environment;
  auto EnvBlockWide =
      std::unique_ptr<wchar_t[], decltype(&FreeEnvironmentStringsW)>(
          GetEnvironmentStringsW(), FreeEnvironmentStringsW);
  if (!EnvBlockWide)
    return;

  // Convert wide env block to UTF-8 once.
  size_t EnvBlockLen = 0;
  while (EnvBlockWide[EnvBlockLen] != L'\0')
    EnvBlockLen += std::wcslen(&EnvBlockWide[EnvBlockLen]) + 1;
  ++EnvBlockLen;

  std::string EnvBlockUtf8;
  if (!llvm::convertUTF16ToUTF8String(
          llvm::ArrayRef<char>(reinterpret_cast<char *>(EnvBlockWide.get()),
                               EnvBlockLen * sizeof(wchar_t)),
          EnvBlockUtf8))
    return;

  // Helper: case-insensitive starts_with for ASCII keys (env var names).
  auto startsWithKeyCI = [](StringRef S, StringRef KeyWithEq) -> bool {
    return S.size() >= KeyWithEq.size() && S.substr(0, KeyWithEq.size()).equals_insensitive(KeyWithEq);
  };

  for (const char *Cursor = EnvBlockUtf8.data(); *Cursor != '\0';) {
    StringRef EnvVar(Cursor);

    // Neutralize a minimal set of VC variables that cause tool mixing.
    // This is less destructive than clearing LIB/INCLUDE.
    if (startsWithKeyCI(EnvVar, "vctoolsinstalldir=") ||
        startsWithKeyCI(EnvVar, "vcinstalldir=") ||
        startsWithKeyCI(EnvVar, "vsinstalldir=") ||
        startsWithKeyCI(EnvVar, "vscmd_ver=") ||
        startsWithKeyCI(EnvVar, "vscmd_arg_tgt_arch=") ||
        startsWithKeyCI(EnvVar, "vscmd_arg_host_arch=") ||
        startsWithKeyCI(EnvVar, "platform=")) {
      // Drop it by not copying; add empty override later if you want.
      Cursor += EnvVar.size() + 1;
      continue;
    }

    // Default: keep as-is.
    Environment.push_back(Args.MakeArgString(EnvVar));
    Cursor += EnvVar.size() + 1;
  }

  auto LinkCmd = std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(LinkPath), CmdArgs, Inputs, Output);

  if (!Environment.empty())
    LinkCmd->setEnvironment(Environment);

  C.addCommand(std::move(LinkCmd));
}

WindowsItaniumToolChain::WindowsItaniumToolChain(const Driver &D,
                                                 const llvm::Triple &Triple,
                                                 const ArgList &Args)
    : ToolChain(D, Triple, Args), CudaInstallation(D, Triple, Args),
      RocmInstallation(D, Triple, Args), SYCLInstallation(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);

  std::optional<llvm::StringRef> VCToolsDir, VCToolsVersion;
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_vctoolsdir))
    VCToolsDir = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_vctoolsversion))
    VCToolsVersion = A->getValue();

  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkdir))
    WinSdkDir = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkversion))
    WinSdkVersion = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsysroot))
    WinSysRoot = A->getValue();

  llvm::findVCToolChainViaCommandLine(getVFS(), VCToolsDir, VCToolsVersion,
                                      WinSysRoot, VCToolChainPath, VSLayout) ||
      llvm::findVCToolChainViaEnvironment(getVFS(), VCToolChainPath,
                                          VSLayout) ||
      llvm::findVCToolChainViaSetupConfig(getVFS(), VCToolsVersion,
                                          VCToolChainPath, VSLayout) ||
      llvm::findVCToolChainViaRegistry(VCToolChainPath, VSLayout);

  // Auto-detect SDK if explicit paths provided or environment not set.
  bool hasExplicitSDKArgs = WinSdkDir.has_value() ||
                            WinSdkVersion.has_value() || WinSysRoot.has_value();

  if (hasExplicitSDKArgs) {
    llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                           WindowsSDKDir, WindowsSDKMajor,
                           WindowsSDKIncludeVersion, WindowsSDKLibVersion);
  }

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
WindowsItaniumToolChain::getDefaultUnwindTableLevel(const ArgList &Args) const {
  // Enable for architectures where LLVM generates unwind tables.
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::arm ||
      getArch() == llvm::Triple::thumb || getArch() == llvm::Triple::aarch64)
    return UnwindTableLevel::Asynchronous;

  return UnwindTableLevel::None;
}

bool WindowsItaniumToolChain::isPICDefault() const {
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

bool WindowsItaniumToolChain::isPIEDefault(const ArgList &Args) const {
  return false;
}

bool WindowsItaniumToolChain::isPICDefaultForced() const {
  // 64-bit Windows ABIs require position-independent code.
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

SanitizerMask WindowsItaniumToolChain::getSupportedSanitizers() const {
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
WindowsItaniumToolChain::GetExceptionModel(const ArgList &Args) const {
  if (Args.hasArg(options::OPT_fsjlj_exceptions))
    return llvm::ExceptionHandling::SjLj;
  // SEH with Itanium personality on 64-bit; SJLJ on 32-bit.
  // Table-based SEH (DISPATCHER_CONTEXT) only exists on x64/ARM64.
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::aarch64)
    return llvm::ExceptionHandling::WinEH;
  return llvm::ExceptionHandling::SjLj;
}

void WindowsItaniumToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind /*DeviceOffloadKind*/) const {
  // Avoid LTO link errors from available_externally dllimport inlines.
  if (!DriverArgs.hasArg(options::OPT_fno_dllexport_inlines))
    CC1Args.push_back("-fno-dllexport-inlines");

  if (!DriverArgs.hasArg(options::OPT_fno_ms_extensions))
    CC1Args.push_back("-fms-extensions");

  // When targeting UCRT (not llvm-libc), define __MSVCRT__ so headers and
  // libraries know the C runtime provides underscore-prefixed functions
  // (_access, _open, _vsnprintf, etc.).  Mirrors MinGW's convention.
  if (!usesLLVMLibC()) {
    CC1Args.push_back("-D__MSVCRT__");
  }

  // llvm-libc uses 32-bit wchar_t for full POSIX compliance (UTF-32,
  // matching Linux/macOS). UCRT retains the standard Windows 16-bit
  // wchar_t (UTF-16).
  if (usesLLVMLibC() &&
      !DriverArgs.hasArg(options::OPT_fshort_wchar,
                         options::OPT_fno_short_wchar)) {
    CC1Args.push_back("-fwchar-type=int");
    CC1Args.push_back("-fsigned-wchar");
    // L"..." produces char16_t[] so SDK headers (which use L"..." for
    // UTF-16 strings) remain compatible with the 16-bit Win32 WCHAR ABI.
    if (!DriverArgs.hasFlag(options::OPT_fno_wide_char16_literals,
                            options::OPT_fwide_char16_literals, false))
      CC1Args.push_back("-fwide-char16-literals");
  }

  // ISO-conforming wide specifiers for wprintf/wscanf (%s = char*, %ls = wchar_t*).
  // Auto-links iso_stdio_wide_specifiers.lib via #pragma comment(lib, ...).
  CC1Args.push_back("-D_CRT_STDIO_ISO_WIDE_SPECIFIERS");

  // Suppress MSVC "insecure function" deprecation warnings for standard C.
  CC1Args.push_back("-D_CRT_SECURE_NO_WARNINGS");

  // Signals its safe to export UCRT functions through C++ modules
  CC1Args.push_back("-D_STATIC_INLINE_UCRT_FUNCTIONS=0");

  // Windows lacks sys/time.h.
  CC1Args.push_back("-UCLOCK_REALTIME");

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

  // Linker-only options; claim to suppress unused warnings.
  for (auto Opt : {options::OPT_mwindows, options::OPT_mconsole}) {
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

  // -nostdlibinc suppresses C library headers (UCRT / llvm-libc) but not
  // Windows SDK headers (shared/, um/).  Runtimes built with llvm-libc pass
  // -nostdlibinc to avoid UCRT; they still need SDK headers for SEH types.
  if (!DriverArgs.hasArg(options::OPT_nostdlibinc)) {
    if (usesLLVMLibC()) {
      auto AddInstalledLibCIncludes = [&](const llvm::Twine &Root) {
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
        AddInstalledLibCIncludes(getDriver().SysRoot);

      llvm::SmallString<128> DriverRoot(getDriver().Dir);
      llvm::sys::path::append(DriverRoot, "..");
      AddInstalledLibCIncludes(DriverRoot);

      for (const std::string &LibPath : getFilePaths()) {
        llvm::SmallString<128> Root(LibPath);
        llvm::sys::path::append(Root, "..");
        AddInstalledLibCIncludes(Root);
      }
    } else if (useUniversalCRT()) {
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
  // When using LLVM libc, <sys/ntabi.h> provides the SEH/CONTEXT types that
  // runtimes need, so the SDK include paths are not added.
  std::string WindowsSDKDir;
  int Major = 0;
  std::string IncludeVer;
  std::string LibVer;
  if (!usesLLVMLibC() &&
      llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
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

  // VC headers last so they don't override UCRT/Windows SDK headers.
  if (GetRuntimeLibType(DriverArgs) == ToolChain::RLT_Msvcrt &&
      !VCToolChainPath.empty()) {
    addSystemInclude(
        DriverArgs, CC1Args,
        llvm::getSubDirectoryPath(llvm::SubDirectoryType::Include, VSLayout,
                                  VCToolChainPath, getArch()));
  }
}

void WindowsItaniumToolChain::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  // Claim -stdlib= to suppress "unused during compilation" warning.
  // Windows Itanium always uses libc++.
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

ToolChain::CXXStdlibType
WindowsItaniumToolChain::GetCXXStdlibType(const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++") {
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

ToolChain::RuntimeLibType
WindowsItaniumToolChain::GetDefaultRuntimeLibType() const {
  // Configurable via -DCLANG_WIN32_ITANIUM_DEFAULT_RTLIB at build time.
  StringRef DefaultRtlib = CLANG_WIN32_ITANIUM_DEFAULT_RTLIB;
  if (DefaultRtlib == "compiler-rt")
    return ToolChain::RLT_CompilerRT;
  return ToolChain::RLT_Msvcrt;
}

LibCMode WindowsItaniumToolChain::GetDefaultLibCMode() const {
  StringRef DefaultLibC = CLANG_WIN32_ITANIUM_DEFAULT_LIBC;
  if (DefaultLibC == "llvm-libc")
    return LibCMode::LLVMLibC;
  return LibCMode::System;
}

void WindowsItaniumToolChain::AddCXXStdlibLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  CmdArgs.push_back("c++.lib");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("c++experimental.lib");
}

Tool *WindowsItaniumToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

void WindowsItaniumToolChain::AddSystemIncludeWithSubfolder(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    const std::string &Folder, const Twine &Sub1,
    const Twine &Sub2, const Twine &Sub3) const {
  llvm::SmallString<128> P(Folder);
  llvm::sys::path::append(P, Sub1, Sub2, Sub3);
  addSystemInclude(DriverArgs, CC1Args, P);
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

void WindowsItaniumToolChain::AddCudaIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  CudaInstallation->AddCudaIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::AddHIPIncludeArgs(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args) const {
  RocmInstallation->AddHIPIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::addSYCLIncludeArgs(const ArgList &DriverArgs,
                                                 ArgStringList &CC1Args) const {
  SYCLInstallation->addSYCLIncludeArgs(DriverArgs, CC1Args);
}

void WindowsItaniumToolChain::addOffloadRTLibs(unsigned ActiveKinds,
                                               const ArgList &Args,
                                               ArgStringList &CmdArgs) const {
  if (Args.hasArg(options::OPT_no_hip_rt) || Args.hasArg(options::OPT_r))
    return;

  if (ActiveKinds & Action::OFK_HIP) {
    CmdArgs.append({Args.MakeArgString(StringRef("-libpath:") +
                                       RocmInstallation->getLibPath()),
                    "amdhip64.lib"});
  }
}

void WindowsItaniumToolChain::printVerboseInfo(raw_ostream &OS) const {
  CudaInstallation->print(OS);
  RocmInstallation->print(OS);

  if (FoundWindowsSDK()) {
    OS << "Windows SDK: " << WindowsSDKDir;
    if (!WindowsSDKIncludeVersion.empty())
      OS << " (version " << WindowsSDKIncludeVersion << ")";
    OS << "\n";
  } else if (llvm::sys::Process::GetEnv("INCLUDE").has_value()) {
    OS << "Windows SDK: using INCLUDE/LIB environment variables\n";
  }
}
