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
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
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

// Translate MSVC-style /O flags to clang equivalents.
// This allows users familiar with MSVC to use /O2, /Od, etc.
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
        // Don't warn about /Oy- in x86-64 builds (where
        // SupportsForcingFramePointer is false). The flag having no effect
        // there is a compiler-internal optimization, and people shouldn't have
        // to special-case their build files for x86-64.
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
      // DWARF and WASM exceptions are not supported on Windows Itanium.
      // SEH exceptions with Itanium personality are the default.
      getDriver().Diag(diag::warn_drv_unsupported_option_for_target)
          << A->getAsString(Args) << getTriple().str();
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_fseh_exceptions));
    } else if (A->getOption().matches(options::OPT_mthreads)) {
      // -mthreads is a MinGW-specific flag that links mingwthrd for thread-safe
      // exception handling. Windows Itanium uses Win32 threads via the MSVC
      // runtime which is already thread-safe, so this flag has no effect.
      // Mark as ignored to prevent "unsupported option for target" error.
      // The driver will emit "argument unused" warning automatically.
      A->ignoreTargetSpecific();
    } else if (OFK != Action::OFK_HIP) {
      // HIP Toolchain translates input args by itself.
      DAL->append(A);
    }
  }

  return DAL;
}

void tools::windowsitanium::Linker::ConstructJob(
    Compilation &C, const JobAction &JA, const InputInfo &Output,
    const InputInfoList &Inputs, const ArgList &Args,
    const char *LinkingOutput) const {
  // Silence warning for "clang -g foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_g_Group);
  // and "clang -emit-llvm foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  // and for "clang -w foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_w);

  const auto &TC = static_cast<const WindowsItaniumToolChain &>(getToolChain());
  const llvm::Triple &T = TC.getTriple();
  const Driver &D = TC.getDriver();
  ArgStringList CmdArgs;

  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(Twine("-out:") + Output.getFilename()));

  // Machine type for COFF linker. Explicitly specify the machine type for all
  // architectures rather than letting the linker infer it from object files.
  // This provides clearer error messages and avoids potential mismatches.
  // Handle ARM64X and ARM64EC variants specially as they have unique semantics.
  if (Args.hasArg(options::OPT_marm64x)) {
    CmdArgs.push_back("-machine:arm64x");
  } else if (TC.getTriple().isWindowsArm64EC()) {
    CmdArgs.push_back("-machine:arm64ec");
  } else {
    switch (TC.getArch()) {
    default:
      D.Diag(diag::err_target_unknown_triple) << TC.getEffectiveTriple().str();
      return;
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
      CmdArgs.push_back("-machine:arm");
      break;
    case llvm::Triple::aarch64:
      CmdArgs.push_back("-machine:arm64");
      break;
    case llvm::Triple::x86:
      CmdArgs.push_back("-machine:x86");
      break;
    case llvm::Triple::x86_64:
      CmdArgs.push_back("-machine:x64");
      break;
    }
  }

  // Handle vector math libraries.
  if (const Arg *A = Args.getLastArg(options::OPT_fveclib)) {
    StringRef V = A->getValue();
    if (V == "ArmPL")
      CmdArgs.push_back(Args.MakeArgString("--dependent-lib=amath"));
  }

  // Enable auto-import for vtable pseudo-relocations. This is required for
  // the Itanium ABI on Windows where vtable pointers cannot be stored directly
  // in PE/COFF due to DLL runtime address indirection through the IAT.
  // See: https://llvm.org/docs/HowToBuildWindowsItaniumPrograms.html
  CmdArgs.push_back("-auto-import");

  // Disable incremental linking. Auto-import creates pseudo-relocations that
  // are resolved at runtime, which is incompatible with incremental linking.
  CmdArgs.push_back("-incremental:no");

  // Handle subsystem selection. Default to console for executables.
  // Users can override with -Wl,-subsystem:windows or similar.
  bool isDLL = Args.hasArg(options::OPT_shared);
  if (!isDLL) {
    // Check for -mwindows/-mconsole flags.
    Arg *SubsysArg =
        Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
    if (SubsysArg && SubsysArg->getOption().matches(options::OPT_mwindows))
      CmdArgs.push_back("-subsystem:windows");
    else
      CmdArgs.push_back("-subsystem:console");
  }

  // Handle DLL builds.
  if (isDLL) {
    CmdArgs.push_back("-dll");

    // Generate import library.
    SmallString<128> ImplibName(Output.getFilename());
    llvm::sys::path::replace_extension(ImplibName, "lib");
    CmdArgs.push_back(Args.MakeArgString("-implib:" + ImplibName));

    // Set DLL entry point. The CRT startup stub calls DllMain after
    // initialization. On x86, the symbol is decorated with @12 for the
    // 12 bytes of __stdcall parameters (HINSTANCE, DWORD, LPVOID).
    StringRef entryPoint;
    switch (T.getArch()) {
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
    // Set executable entry point based on subsystem and application type.
    // The entry point must match the CRT startup object being linked.
    if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
      Arg *SubsysArg =
          Args.getLastArg(options::OPT_mwindows, options::OPT_mconsole);
      if (SubsysArg && SubsysArg->getOption().matches(options::OPT_mwindows)) {
        // GUI application - use WinMainCRTStartup
        CmdArgs.push_back("-entry:WinMainCRTStartup");
      } else {
        // Console application - use mainCRTStartup
        CmdArgs.push_back("-entry:mainCRTStartup");
      }
    }
  }

  // Add debug flag to linker if debug info is requested.
  // Exclude -g0 which explicitly disables debug info.
  if (const Arg *A =
          Args.getLastArg(options::OPT_g_Group, options::OPT__SLASH_Z7))
    if (!A->getOption().matches(options::OPT_g0))
      CmdArgs.push_back("-debug");

  // If we specify /hotpatch, let the linker add padding in front of each
  // function, like MSVC does.
  if (Args.hasArg(options::OPT_fms_hotpatch, options::OPT__SLASH_hotpatch))
    CmdArgs.push_back("-functionpadmin");

  // Pass on /Brepro if it was passed to the compiler.
  // Note that /Brepro maps to -mno-incremental-linker-compatible.
  if (!Args.hasFlag(options::OPT_mincremental_linker_compatible,
                    options::OPT_mno_incremental_linker_compatible,
                    /*Default=*/true))
    CmdArgs.push_back("-Brepro");

  // Control Flow Guard checks. Support both MSVC-style /guard: and the
  // cross-platform -mguard= flag for enabling CFG instrumentation.
  if (Arg *A = Args.getLastArg(options::OPT_mguard_EQ)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs == "cf" || GuardArgs == "cf-nochecks")
      CmdArgs.push_back("-guard:cf");
    else if (GuardArgs == "none")
      CmdArgs.push_back("-guard:cf-");
  }
  for (const Arg *A : Args.filtered(options::OPT__SLASH_guard)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs.equals_insensitive("cf") ||
        GuardArgs.equals_insensitive("cf,nochecks")) {
      CmdArgs.push_back("-guard:cf");
    } else if (GuardArgs.equals_insensitive("cf-")) {
      CmdArgs.push_back("-guard:cf-");
    } else if (GuardArgs.equals_insensitive("ehcont")) {
      CmdArgs.push_back("-guard:ehcont");
    } else if (GuardArgs.equals_insensitive("ehcont-")) {
      CmdArgs.push_back("-guard:ehcont-");
    }
  }

  CmdArgs.push_back("-nologo");

  // Add DIA SDK library path if requested. The DIA SDK provides COM interfaces
  // for reading debug information (PDB files) and is used by debugging tools.
  // cl.exe doesn't find this automatically, so explicit flags are required.
  if (const Arg *A = Args.getLastArg(options::OPT__SLASH_diasdkdir,
                                     options::OPT__SLASH_winsysroot)) {
    llvm::SmallString<128> DIAPath(A->getValue());
    if (A->getOption().getID() == options::OPT__SLASH_winsysroot)
      llvm::sys::path::append(DIAPath, "DIA SDK");

    // The DIA SDK always uses the legacy vc arch, even in new MSVC versions.
    llvm::sys::path::append(DIAPath, "lib",
                            llvm::archToLegacyVCArch(TC.getArch()));
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") + DIAPath));
  }

  // Add library search paths from -L options.
  for (const auto &LibPath : Args.getAllArgValues(options::OPT_L))
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + LibPath));

  // Add library search paths from LIB environment variable, unless the user
  // expressly set Windows SDK options. This matches MSVC driver behavior.
  bool hasExplicitSDKArgs = Args.hasArg(options::OPT__SLASH_winsdkdir,
                                        options::OPT__SLASH_winsdkversion,
                                        options::OPT__SLASH_winsysroot);
  if (!hasExplicitSDKArgs) {
    if (std::optional<std::string> LibEnv = llvm::sys::Process::GetEnv("LIB")) {
      SmallVector<StringRef, 8> Paths;
      StringRef(*LibEnv).split(Paths, ';', -1, false);
      for (StringRef Path : Paths)
        CmdArgs.push_back(Args.MakeArgString("-libpath:" + Path));
    }
  }

  // If explicit SDK flags were provided or LIB env var isn't set, add
  // auto-detected SDK library paths.
  if (hasExplicitSDKArgs || !llvm::sys::Process::GetEnv("LIB")) {
    // Universal CRT library path.
    std::string UCRTLibPath;
    if (TC.getUniversalCRTLibraryPath(Args, UCRTLibPath))
      CmdArgs.push_back(Args.MakeArgString("-libpath:" + UCRTLibPath));

    // Windows SDK library path.
    std::string SDKLibPath;
    if (TC.getWindowsSDKLibraryPath(Args, SDKLibPath))
      CmdArgs.push_back(Args.MakeArgString("-libpath:" + SDKLibPath));
  }

  // Add toolchain library paths.
  for (const std::string &LibPath : TC.getLibraryPaths()) {
    if (TC.getVFS().exists(LibPath))
      CmdArgs.push_back(Args.MakeArgString("-libpath:" + LibPath));
  }
  for (const std::string &LibPath : TC.getFilePaths())
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + LibPath));

  // Add the compiler-rt library directory to help the linker find
  // sanitizer and other runtime libraries.
  auto CRTPath = TC.getCompilerRTPath();
  if (TC.getVFS().exists(CRTPath))
    CmdArgs.push_back(Args.MakeArgString("-libpath:" + CRTPath));

  // Add inputs - convert -l options to COFF library format.
  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }

    const Arg &A = Input.getInputArg();
    if (A.getOption().matches(options::OPT_l)) {
      StringRef Lib = A.getValue();
      if (Lib.ends_with(".lib"))
        CmdArgs.push_back(Args.MakeArgString(Lib));
      else
        CmdArgs.push_back(Args.MakeArgString(Lib + ".lib"));
      continue;
    }

    // Pass through other linker input options.
    A.renderAsInput(Args, CmdArgs);
  }

  // LTO support. Since we use lld-link (COFF mode), follow MSVC patterns.
  if (D.isUsingLTO()) {
    // Pass sample profile to LTO backend.
    if (Arg *A = tools::getLastProfileSampleUseArg(Args))
      CmdArgs.push_back(
          Args.MakeArgString(Twine("-lto-sample-profile:") + A->getValue()));

    // Split-dwarf support for LTO debugging.
    if (Args.hasFlag(options::OPT_gsplit_dwarf, options::OPT_gno_split_dwarf,
                     false))
      CmdArgs.push_back(Args.MakeArgString(Twine("-dwodir:") +
                                           Output.getFilename() + "_dwo"));
  }

  // VFS overlay support for lld-link.
  for (Arg *A : Args.filtered(options::OPT_vfsoverlay))
    CmdArgs.push_back(
        Args.MakeArgString(Twine("-vfsoverlay:") + A->getValue()));

  // Pass through options specified via /link.
  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);

  if (TC.getSanitizerArgs(Args).needsFuzzer()) {
    if (!Args.hasArg(options::OPT_shared))
      CmdArgs.push_back(Args.MakeArgString(
          Twine("-wholearchive:") + TC.getCompilerRTArgString(Args, "fuzzer")));
    CmdArgs.push_back("-debug");
    // Prevent the linker from padding sections used for instrumentation arrays.
    CmdArgs.push_back("-incremental:no");
  }

  // Address Sanitizer support. Windows Itanium uses the dynamic Universal CRT
  // (ucrt), so we always use the dynamic ASan runtime thunk rather than the
  // static thunk used by MSVC's /MT option.
  if (TC.getSanitizerArgs(Args).needsAsanRt()) {
    CmdArgs.push_back("-debug");
    CmdArgs.push_back("-incremental:no");
    CmdArgs.push_back(TC.getCompilerRTArgString(Args, "asan_dynamic"));
    // Make sure the linker considers all object files from the dynamic
    // runtime thunk.
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-wholearchive:") +
        TC.getCompilerRT(Args, "asan_dynamic_runtime_thunk")));
    // Ensure the ASan SEH interceptor is not optimized out at link time
    // for proper structured exception handling support.
    CmdArgs.push_back(Args.MakeArgString(
        TC.getArch() == llvm::Triple::x86 ? "-include:___asan_seh_interceptor"
                                          : "-include:__asan_seh_interceptor"));
  }

  // OpenMP support. Use LLVM's libomp rather than MSVC's vcomp.
  if (Args.hasFlag(options::OPT_fopenmp, options::OPT_fopenmp_EQ,
                   options::OPT_fno_openmp, false)) {
    CmdArgs.push_back("-nodefaultlib:vcomp.lib");
    CmdArgs.push_back("-nodefaultlib:vcompd.lib");
    CmdArgs.push_back(Args.MakeArgString(Twine("-libpath:") +
                                         TC.getDriver().Dir + "/../lib"));
    switch (D.getOpenMPRuntime(Args)) {
    case Driver::OMPRT_OMP:
      CmdArgs.push_back("-defaultlib:libomp.lib");
      break;
    case Driver::OMPRT_IOMP5:
      CmdArgs.push_back("-defaultlib:libiomp5md.lib");
      break;
    case Driver::OMPRT_GOMP:
      break;
    case Driver::OMPRT_Unknown:
      // Already diagnosed.
      break;
    }
  }

  // Fortran (Flang) runtime support.
  if (D.IsFlangMode() &&
      !Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    TC.addFortranRuntimeLibraryPath(Args, CmdArgs);
    TC.addFortranRuntimeLibs(Args, CmdArgs);
    // Fortran programs use 'main' as entry point defined in Flang's runtime.
    if (!isDLL)
      CmdArgs.push_back("-subsystem:console");
  }

  // CRT startup handling depends on the runtime library type.
  //
  // When using compiler-rt (-rtlib=compiler-rt, which is the default for
  // Windows Itanium), we use CRT startup objects from compiler-rt that provide
  // the entry points (mainCRTStartup, etc.) and initialize the C runtime.
  //
  // When NOT using compiler-rt (-rtlib=platform or -rtlib=libgcc), we fall back
  // to linking msvcrt.lib which provides the entry points from the MSVC runtime.
  // Note: msvcrt.lib pulls in vcruntime which has MSVC C++ ABI symbols, so this
  // mode should only be used when building C code or when ABI conflicts are
  // acceptable.
  //
  // Users can opt out with -nostartfiles or -nostdlib.
  bool UseCompilerRT =
      TC.GetRuntimeLibType(Args) == ToolChain::RLT_CompilerRT;

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    if (UseCompilerRT) {
      // Using compiler-rt: link CRT startup objects from compiler-rt
      std::string CRTVariant;
      if (isDLL) {
        CRTVariant = "dllmain";
      } else if (Args.hasArg(options::OPT_mwindows)) {
        // -mwindows implies GUI application with WinMain
        // Use wWinMain if Unicode entry point is detected, otherwise WinMain
        // For now, default to narrow WinMain; users can override with
        // explicit entry point or by linking their own CRT object
        CRTVariant = "winmain";
      } else {
        // Console application - use main or wmain
        // Default to main; wmain would need explicit user request
        CRTVariant = "main";
      }

      // Try to find the CRT object in compiler-rt
      std::string CRTObj =
          TC.getCompilerRT(Args, "crt_" + CRTVariant, ToolChain::FT_Object);
      if (TC.getVFS().exists(CRTObj)) {
        CmdArgs.push_back(Args.MakeArgString(CRTObj));
      }
    }
  }

  // Handle default libraries. Use -defaultlib: format like MSVC for consistency
  // and to ensure proper library ordering by the linker.
  // Skip in CL mode as the user is expected to handle libraries explicitly.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs) &&
      !D.IsCLMode()) {
    // C++ standard library - libc++ is the only supported option.
    if (TC.ShouldLinkCXXStdlib(Args)) {
      CmdArgs.push_back("-defaultlib:c++");
      if (Args.hasArg(options::OPT_fexperimental_library))
        CmdArgs.push_back("-defaultlib:c++experimental");
    }

    // Unwind library for exception handling.
    CmdArgs.push_back("-defaultlib:unwind");

    // C Runtime libraries.
    // ucrt (Universal C Runtime) provides C library functions (printf, etc.)
    // msvcrt provides CRT startup code (mainCRTStartup, etc.) when not using
    // compiler-rt CRT objects.
    CmdArgs.push_back("-defaultlib:ucrt");
    if (!UseCompilerRT) {
      // When NOT using compiler-rt, we need msvcrt for entry points.
      // msvcrt.lib provides the CRT startup code (mainCRTStartup, etc.)
      // Note: This also pulls in vcruntime which has some MSVC C++ ABI symbols.
      CmdArgs.push_back("-defaultlib:msvcrt");
    }

    // Legacy stdio definitions for functions like fprintf that are normally
    // inlined in MSVC headers but need library definitions when using
    // -D_NO_CRT_STDIO_INLINE.
    CmdArgs.push_back("-defaultlib:legacy_stdio_definitions");

    // POSIX compatibility layer for functions like open(), close(), etc.
    CmdArgs.push_back("-defaultlib:oldnames");

    // Essential Windows API libraries. Unlike MSVC which embeds library
    // references in object files via #pragma comment(lib, ...), we need to
    // link these explicitly. This list matches Visual Studio's default
    // CoreLibraryDependencies from Microsoft.Cpp.CoreWin.props.
    CmdArgs.push_back("-defaultlib:kernel32");
    CmdArgs.push_back("-defaultlib:user32");
    CmdArgs.push_back("-defaultlib:gdi32");
    CmdArgs.push_back("-defaultlib:winspool");
    CmdArgs.push_back("-defaultlib:comdlg32");
    CmdArgs.push_back("-defaultlib:advapi32");
    CmdArgs.push_back("-defaultlib:shell32");
    CmdArgs.push_back("-defaultlib:ole32");
    CmdArgs.push_back("-defaultlib:oleaut32");
    CmdArgs.push_back("-defaultlib:uuid");
    CmdArgs.push_back("-defaultlib:odbc32");
    CmdArgs.push_back("-defaultlib:odbccp32");
  }

  // Add offload runtime libraries for CUDA/HIP.
  TC.addOffloadRTLibs(C.getActiveOffloadKinds(), Args, CmdArgs);

  // Add profile runtime library if needed.
  TC.addProfileRTLibs(Args, CmdArgs);

  // Get linker path. LLD is required for Windows Itanium due to auto-import
  // support; MSVC link.exe cannot be used as it lacks this feature.
  StringRef linkerName = Args.getLastArgValue(options::OPT_fuse_ld_EQ, "lld");
  std::string linkerPath;
  if (linkerName.equals_insensitive("lld") ||
      linkerName.equals_insensitive("lld-link")) {
    linkerPath = TC.GetProgramPath("lld-link");
  } else if (linkerName.equals_insensitive("link")) {
    // link.exe lacks auto-import support required for this target.
    D.Diag(diag::warn_drv_unsupported_option_for_target)
        << "-fuse-ld=link" << TC.getTripleString();
    linkerPath = TC.GetProgramPath("link.exe");
  } else {
    linkerPath = TC.GetProgramPath(linkerName.str().c_str());
  }

  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileUTF16(),
      Args.MakeArgString(linkerPath), CmdArgs, Inputs, Output));
}

WindowsItaniumToolChain::WindowsItaniumToolChain(const Driver &D,
                                                 const llvm::Triple &Triple,
                                                 const ArgList &Args)
    : ToolChain(D, Triple, Args), CudaInstallation(D, Triple, Args),
      RocmInstallation(D, Triple, Args), SYCLInstallation(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().Dir);

  // Parse Windows SDK configuration from command line arguments.
  // These allow explicit SDK path specification without relying on
  // vcvarsall.bat.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkdir))
    WinSdkDir = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsdkversion))
    WinSdkVersion = A->getValue();
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_winsysroot))
    WinSysRoot = A->getValue();

  // If explicit SDK paths are provided, or if environment variables aren't set,
  // try to auto-detect the Windows SDK. This provides better diagnostics and
  // enables cross-compilation scenarios without vcvarsall.bat.
  bool hasExplicitSDKArgs = WinSdkDir.has_value() ||
                            WinSdkVersion.has_value() || WinSysRoot.has_value();
  bool hasEnvVars = llvm::sys::Process::GetEnv("INCLUDE").has_value();

  if (hasExplicitSDKArgs || !hasEnvVars) {
    // Try to detect Windows SDK installation.
    llvm::getWindowsSDKDir(getVFS(), WinSdkDir, WinSdkVersion, WinSysRoot,
                           WindowsSDKDir, WindowsSDKMajor,
                           WindowsSDKIncludeVersion, WindowsSDKLibVersion);
  }

  // Add library paths adjacent to the clang installation.
  // This allows finding libc++, libunwind, etc. that are installed alongside.
  SmallString<128> LibPath(D.Dir);
  llvm::sys::path::append(LibPath, "..", "lib");
  if (getVFS().exists(LibPath))
    getFilePaths().push_back(std::string(LibPath));

  // Also check for target-specific library directory.
  SmallString<128> TargetLibPath(D.Dir);
  llvm::sys::path::append(TargetLibPath, "..", "lib", Triple.str());
  if (getVFS().exists(TargetLibPath))
    getFilePaths().push_back(std::string(TargetLibPath));
}

ToolChain::UnwindTableLevel
WindowsItaniumToolChain::getDefaultUnwindTableLevel(const ArgList &Args) const {
  // All non-x86_32 Windows targets require unwind tables. However, LLVM
  // doesn't know how to generate them for all targets, so only enable
  // the ones that are actually implemented.
  if (getArch() == llvm::Triple::x86_64 || getArch() == llvm::Triple::arm ||
      getArch() == llvm::Triple::thumb || getArch() == llvm::Triple::aarch64)
    return UnwindTableLevel::Asynchronous;

  return UnwindTableLevel::None;
}

bool WindowsItaniumToolChain::isPICDefault() const {
  // PIC is inherent on 64-bit Windows due to RIP-relative addressing.
  return getArch() == llvm::Triple::x86_64 ||
         getArch() == llvm::Triple::aarch64;
}

bool WindowsItaniumToolChain::isPIEDefault(const ArgList &Args) const {
  // PIE is not a Windows concept; ASLR is handled via /DYNAMICBASE.
  return false;
}

bool WindowsItaniumToolChain::isPICDefaultForced() const {
  // On 64-bit Windows (x64 and ARM64), position-independent code is mandatory
  // due to the ABI design: x64 uses RIP-relative addressing, and ARM64 uses
  // ADRP/ADD sequences that require relocations. The linker and loader expect
  // all code to be position-independent on these architectures.
  // On 32-bit x86, non-PIC code is still valid as direct addressing is used.
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
  // Windows Itanium uses SEH-based unwinding with Itanium personality functions.
  // This provides zero-cost exceptions by using Windows' native .pdata/.xdata
  // unwind tables combined with __gxx_personality_seh0 which bridges to the
  // Itanium C++ ABI exception handling in libc++abi.
  //
  // The unwinding flow is:
  //   1. Windows SEH calls __gxx_personality_seh0 (registered via .seh_handler)
  //   2. __gxx_personality_seh0 calls _GCC_specific_handler (libunwind bridge)
  //   3. _GCC_specific_handler invokes Itanium personality with DWARF LSDA
  //
  // SJLJ exceptions (-fsjlj-exceptions) are also supported as a fallback.
  if (Args.hasArg(options::OPT_fsjlj_exceptions))
    return llvm::ExceptionHandling::SjLj;
  return llvm::ExceptionHandling::WinEH;
}

void WindowsItaniumToolChain::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind /*DeviceOffloadKind*/) const {
  // Enable MS extensions to parse MSVC SDK headers. Windows Itanium uses the
  // same headers as MSVC, which require __int64, __pragma, __declspec, etc.
  // These are not enabled by default since the triple is windows-itanium,
  // not windows-msvc.
  if (!DriverArgs.hasArg(options::OPT_fno_ms_extensions))
    CC1Args.push_back("-fms-extensions");

  // MSVC STL kindly allows removing all usages of typeid by defining
  // _HAS_STATIC_RTTI to 0. Do so when compiling with -fno-rtti. This also
  // helps when using MSVC headers with libc++.
  if (DriverArgs.hasFlag(options::OPT_fno_rtti, options::OPT_frtti,
                         /*Default=*/false))
    CC1Args.push_back("-D_HAS_STATIC_RTTI=0");

  // NOTE: We intentionally do NOT enable -fms-compatibility for Windows Itanium.
  // That flag enables permissive semantic behaviors (function ptr to void*,
  // dependent base lookup hacks, etc.) that are workarounds for non-conforming
  // MSVC code. Since Windows Itanium uses the Itanium ABI and targets Clang/GCC
  // semantics, we want third-party code to use standard C++ code paths.
  // -fms-extensions (enabled above) provides the necessary syntax extensions
  // (__declspec, __int64, etc.) for SDK headers without the semantic hacks.

  // Force Itanium ABI in libc++ headers unless user explicitly controls it.
  // This ensures the Itanium name mangling and vtable layout are used instead
  // of the Microsoft ABI.
  bool userDefinedItaniumABI = false;
  for (const auto *A : DriverArgs.filtered(options::OPT_D, options::OPT_U)) {
    StringRef value = A->getValue();
    if (value.starts_with("_LIBCPP_ABI_FORCE_ITANIUM")) {
      userDefinedItaniumABI = true;
      break;
    }
  }
  if (!userDefinedItaniumABI)
    CC1Args.push_back("-D_LIBCPP_ABI_FORCE_ITANIUM");

  // Prevent dllimport from propagating to inline methods of dllimport classes.
  // MSVC-style dllimport causes inline methods to get available_externally
  // linkage, which can cause link errors with LTO when the expected symbol
  // isn't exported from the DLL (e.g., due to ABI tag mismatches with libc++).
  // This makes inline methods stay as linkonce_odr local definitions.
  if (!DriverArgs.hasArg(options::OPT_fno_dllexport_inlines))
    CC1Args.push_back("-fno-dllexport-inlines");

  // Prevent MSVC headers from declaring inline stdio functions that can cause
  // duplicate symbol errors. This requires linking against
  // legacy_stdio_definitions.lib for the library implementations.
  // Use --dependent-lib to embed this requirement in object files, ensuring it
  // works even with -nostdlib (used by runtimes builds).
  CC1Args.push_back("-D_NO_CRT_STDIO_INLINE");
  CC1Args.push_back("--dependent-lib=legacy_stdio_definitions");

  // Windows lacks sys/time.h, so CLOCK_REALTIME is not available.
  // Undefine it to prevent libc++ from attempting to use clock_gettime().
  CC1Args.push_back("-UCLOCK_REALTIME");

  // Control Flow Guard. Handle -mguard= for CFG instrumentation.
  if (Arg *A = DriverArgs.getLastArg(options::OPT_mguard_EQ)) {
    StringRef GuardArgs = A->getValue();
    if (GuardArgs == "cf") {
      // Emit CFG instrumentation and the table of address-taken functions.
      CC1Args.push_back("-cfguard");
    } else if (GuardArgs == "cf-nochecks") {
      // Emit only the table of address-taken functions.
      CC1Args.push_back("-cfguard-no-checks");
    } else if (GuardArgs != "none") {
      getDriver().Diag(diag::err_drv_unsupported_option_argument)
          << A->getSpelling() << GuardArgs;
    }
  }

  // Mark target-specific options as used to suppress warnings. These options
  // are handled by the linker rather than the compiler frontend.
  // Note: -mthreads is handled in TranslateArgs where it's fully ignored.
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

  const Driver &D = getDriver();

  // Clang builtin headers.
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> ResourceDir(D.ResourceDir);
    llvm::sys::path::append(ResourceDir, "include");
    addSystemInclude(DriverArgs, CC1Args, ResourceDir);
  }

  // Add %INCLUDE%-like directories from the -imsvc flag.
  for (const auto &Path : DriverArgs.getAllArgValues(options::OPT__SLASH_imsvc))
    addSystemInclude(DriverArgs, CC1Args, Path);

  // Add system includes from environment variables specified via /external:env:
  for (const auto &Var :
       DriverArgs.getAllArgValues(options::OPT__SLASH_external_env)) {
    if (auto Val = llvm::sys::Process::GetEnv(Var)) {
      SmallVector<StringRef, 8> Dirs;
      StringRef(*Val).split(Dirs, ';', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
      for (StringRef Dir : Dirs)
        addSystemInclude(DriverArgs, CC1Args, Dir);
    }
  }

  // Add DIA SDK include path if requested. The DIA SDK provides COM interfaces
  // for reading debug information (PDB files). cl.exe doesn't find this
  // automatically, so explicit flags are required via /diasdkdir or
  // /winsysroot.
  if (const Arg *A = DriverArgs.getLastArg(options::OPT__SLASH_diasdkdir,
                                           options::OPT__SLASH_winsysroot)) {
    llvm::SmallString<128> DIASDKPath(A->getValue());
    if (A->getOption().getID() == options::OPT__SLASH_winsysroot)
      llvm::sys::path::append(DIASDKPath, "DIA SDK");
    AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, std::string(DIASDKPath),
                                  "include");
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // Helper to add includes from an environment variable.
  auto AddSystemIncludesFromEnv = [&](StringRef Var) -> bool {
    if (auto Val = llvm::sys::Process::GetEnv(Var)) {
      SmallVector<StringRef, 8> Dirs;
      StringRef(*Val).split(Dirs, ';', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
      if (!Dirs.empty()) {
        addSystemIncludes(DriverArgs, CC1Args, Dirs);
        return true;
      }
    }
    return false;
  };

  // Honor %INCLUDE% and %EXTERNAL_INCLUDE%. These should have essential search
  // paths set by vcvarsall.bat. Skip if the user expressly set any of the
  // Windows SDK options, as they want explicit control over include paths.
  // This matches MSVC driver behavior.
  if (!DriverArgs.hasArg(options::OPT__SLASH_winsysroot,
                         options::OPT__SLASH_winsdkdir,
                         options::OPT__SLASH_winsdkversion)) {
    bool found = AddSystemIncludesFromEnv("INCLUDE");
    found |= AddSystemIncludesFromEnv("EXTERNAL_INCLUDE");
    if (found)
      return;
  }

  // If environment variables aren't set or explicit SDK flags were provided,
  // use auto-detected Windows SDK paths.
  if (FoundWindowsSDK()) {
    // Universal CRT headers.
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

    // Windows SDK headers.
    std::string includeVersion = WindowsSDKIncludeVersion;
    if (WindowsSDKMajor >= 10)
      if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
          WinSdkVersion.has_value())
        includeVersion = *WinSdkVersion;

    if (WindowsSDKMajor >= 8) {
      // Note: includeVersion is empty for SDKs prior to v10.
      // llvm::sys::path::append handles empty strings correctly.
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", includeVersion, "shared");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", includeVersion, "um");
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", includeVersion, "winrt");
      if (WindowsSDKMajor >= 10) {
        llvm::VersionTuple Tuple;
        // C++/WinRT headers were added in SDK version 10.0.17134.0.
        if (!Tuple.tryParse(includeVersion) &&
            Tuple.getSubminor().value_or(0) >= 17134) {
          AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                        "Include", includeVersion, "cppwinrt");
        }
      }
    } else {
      AddSystemIncludeWithSubfolder(DriverArgs, CC1Args, WindowsSDKDir,
                                    "Include", "", "");
    }
  }
}

void WindowsItaniumToolChain::AddClangCXXStdlibIncludeArgs(
    const ArgList &DriverArgs, ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdincxx,
                        options::OPT_nostdlibinc))
    return;

  const Driver &D = getDriver();

  // Search for libc++ headers in multiple locations for robustness.
  // The search order prioritizes target-specific paths to support multi-target
  // installations, then falls back to generic locations.

  // 1. Target-specific path adjacent to clang (for multi-target installations).
  //    e.g., <clang>/include/x86_64-unknown-windows-itanium/c++/v1
  SmallString<128> TargetPath(D.Dir);
  llvm::sys::path::append(TargetPath, "..", "include", getTripleString());
  llvm::sys::path::append(TargetPath, "c++", "v1");
  if (D.getVFS().exists(TargetPath))
    addSystemInclude(DriverArgs, CC1Args, TargetPath);

  // 2. Standard path adjacent to clang installation.
  //    e.g., <clang>/include/c++/v1
  SmallString<128> InstallPath(D.Dir);
  llvm::sys::path::append(InstallPath, "..", "include", "c++", "v1");
  if (D.getVFS().exists(InstallPath))
    addSystemInclude(DriverArgs, CC1Args, InstallPath);

  // 3. Check in library paths - libc++ may be installed alongside libraries.
  //    This handles cases where headers are bundled with the library install.
  for (const std::string &LibPath : getFilePaths()) {
    SmallString<128> LibIncludePath(LibPath);
    llvm::sys::path::append(LibIncludePath, "..", "include", "c++", "v1");
    if (D.getVFS().exists(LibIncludePath)) {
      addSystemInclude(DriverArgs, CC1Args, LibIncludePath);
      break; // Use the first found
    }
  }

  // 4. Search in sysroot for cross-compilation.
  //    e.g., <sysroot>/include/c++/v1
  if (!D.SysRoot.empty()) {
    SmallString<128> SysrootPath(D.SysRoot);
    llvm::sys::path::append(SysrootPath, "include", "c++", "v1");
    if (D.getVFS().exists(SysrootPath))
      addSystemInclude(DriverArgs, CC1Args, SysrootPath);
  }
}

ToolChain::CXXStdlibType
WindowsItaniumToolChain::GetCXXStdlibType(const ArgList &Args) const {
  // Claim the -stdlib= argument to avoid unused argument warnings.
  // libc++ is the only supported option for Windows Itanium.
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++") {
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
          << A->getAsString(Args);
    }
  }
  return ToolChain::CST_Libcxx;
}

void WindowsItaniumToolChain::AddCXXStdlibLibArgs(
    const ArgList &Args, ArgStringList &CmdArgs) const {
  // libc++ is the only supported C++ standard library for Windows Itanium.
  CmdArgs.push_back("-lc++");
  if (Args.hasArg(options::OPT_fexperimental_library))
    CmdArgs.push_back("-lc++experimental");
}

VersionTuple
WindowsItaniumToolChain::computeMSVCVersion(const Driver *D,
                                            const ArgList &Args) const {
  // Check for explicit version arguments first.
  VersionTuple MSVT = ToolChain::computeMSVCVersion(D, Args);
  if (!MSVT.empty())
    return MSVT;

  // Windows Itanium uses MSVC headers, so provide a reasonable default
  // MSVC compatibility version when -fms-extensions is enabled.
  // Use 19.33 (VS 2022 17.3) as the default, matching MSVC toolchain.
  //
  // Note: _MSC_VER is marked as system-header-only for Windows Itanium
  // (see OSTargets.cpp), so it will be visible in SDK headers but not
  // in third-party code like zstd/zlib, ensuring they use GCC/Clang code paths.
  if (Args.hasFlag(options::OPT_fms_extensions, options::OPT_fno_ms_extensions,
                   true))
    return VersionTuple(19, 33);

  return VersionTuple();
}

Tool *WindowsItaniumToolChain::buildLinker() const {
  return new tools::windowsitanium::Linker(*this);
}

Tool *WindowsItaniumToolChain::buildAssembler() const {
  return new tools::ClangAs(*this);
}

void WindowsItaniumToolChain::AddSystemIncludeWithSubfolder(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    const std::string &Folder, const Twine &Subfolder1, const Twine &Subfolder2,
    const Twine &Subfolder3) const {
  SmallString<128> Path(Folder);
  llvm::sys::path::append(Path, Subfolder1, Subfolder2, Subfolder3);
  addSystemInclude(DriverArgs, CC1Args, Path);
}

bool WindowsItaniumToolChain::getWindowsSDKLibraryPath(
    const ArgList &Args, std::string &Path) const {
  Path.clear();

  // First check if we have a cached SDK path from auto-detection.
  if (WindowsSDKDir.empty())
    return false;

  SmallString<128> LibPath(WindowsSDKDir);
  llvm::sys::path::append(LibPath, "Lib");

  // Handle SDK version override for SDK 10+.
  std::string libVersion = WindowsSDKLibVersion;
  if (WindowsSDKMajor >= 10)
    if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
        WinSdkVersion.has_value())
      libVersion = *WinSdkVersion;

  if (WindowsSDKMajor >= 8)
    llvm::sys::path::append(LibPath, libVersion, "um");

  return llvm::appendArchToWindowsSDKLibPath(WindowsSDKMajor, LibPath,
                                             getArch(), Path);
}

bool WindowsItaniumToolChain::getUniversalCRTLibraryPath(
    const ArgList &Args, std::string &Path) const {
  Path.clear();

  std::string UniversalCRTSdkPath;
  std::string UCRTVersion;

  if (!llvm::getUniversalCRTSdkDir(getVFS(), WinSdkDir, WinSdkVersion,
                                   WinSysRoot, UniversalCRTSdkPath,
                                   UCRTVersion))
    return false;

  // Handle SDK version override.
  if (!(WinSdkDir.has_value() || WinSysRoot.has_value()) &&
      WinSdkVersion.has_value())
    UCRTVersion = *WinSdkVersion;

  StringRef ArchName = llvm::archToWindowsSDKArch(getArch());
  if (ArchName.empty())
    return false;

  SmallString<128> LibPath(UniversalCRTSdkPath);
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

  // Print Windows SDK detection status.
  if (FoundWindowsSDK()) {
    OS << "Windows SDK: " << WindowsSDKDir;
    if (!WindowsSDKIncludeVersion.empty())
      OS << " (version " << WindowsSDKIncludeVersion << ")";
    OS << "\n";
  } else if (llvm::sys::Process::GetEnv("INCLUDE").has_value()) {
    OS << "Windows SDK: using INCLUDE/LIB environment variables\n";
  }
}
