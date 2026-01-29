//===-- crt_windows_cmdline.cpp - Command line initialization -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Command line and environment initialization via UCRT delegation.
//
// We delegate to UCRT's __getmainargs/__wgetmainargs functions which:
// - Parse the command line into argc/argv
// - Set up the environment block
// - Populate the standard CRT globals (__argc, __argv, _environ, etc.)
//
// This approach:
// - Avoids ODR conflicts (we use UCRT's symbol definitions)
// - Ensures consistent behavior with MSVC-compiled code
// - Reduces code size and maintenance burden
// - Leverages UCRT's well-tested implementation
//
// UCRT's parsing follows standard Windows conventions:
// - Arguments separated by whitespace
// - Quoted strings preserve spaces: "arg with spaces"
// - Backslash escaping for quotes and backslashes
// - Optional wildcard expansion (we disable it by default)
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32

#include "crt_windows_internal.h"

//===----------------------------------------------------------------------===//
// UCRT imports
//
// These functions and globals are provided by UCRT (ucrtbase.dll or static lib).
// We import them and call __getmainargs/__wgetmainargs during CRT init.
//===----------------------------------------------------------------------===//

// Startup info structure passed to __getmainargs/__wgetmainargs
struct _startupinfo {
  int newmode;  // _newmode flag for _setmode() behavior
};

extern "C" {

// UCRT argument initialization functions
__declspec(dllimport) int __cdecl __getmainargs(
    int* _Argc, char*** _Argv, char*** _Env,
    int _DoWildCard, _startupinfo* _StartInfo);

__declspec(dllimport) int __cdecl __wgetmainargs(
    int* _Argc, wchar_t*** _Argv, wchar_t*** _Env,
    int _DoWildCard, _startupinfo* _StartInfo);

// UCRT-owned globals - we reference these, not define them
// When linking with UCRT, these resolve to UCRT's definitions
__declspec(dllimport) extern int __argc;
__declspec(dllimport) extern char** __argv;
__declspec(dllimport) extern wchar_t** __wargv;
__declspec(dllimport) extern char** _environ;
__declspec(dllimport) extern wchar_t** _wenviron;

}

//===----------------------------------------------------------------------===//
// Internal state
//
// We cache pointers locally for our accessor functions. This allows code
// using crt::getArgv() etc. to work without going through dllimport on
// every access.
//===----------------------------------------------------------------------===//

namespace {

int Argc = 0;
char** Argv = nullptr;
wchar_t** Wargv = nullptr;
char** Environ = nullptr;
wchar_t** Wenviron = nullptr;

// Startup info with default settings
// newmode=0 means _setmode() uses default behavior
_startupinfo StartupInfo = {0};

} // namespace

//===----------------------------------------------------------------------===//
// WinMain command line helpers
//
// For WinMain/wWinMain entry points, we need to provide the command line
// string minus the program name. We parse this directly from GetCommandLine
// since UCRT doesn't provide a separate API for this.
//===----------------------------------------------------------------------===//

namespace {

template <typename CharT>
struct CmdLineTraits;

template <>
struct CmdLineTraits<char> {
  static constexpr char Space = ' ';
  static constexpr char Tab = '\t';
  static constexpr char Quote = '"';
  static CharT* GetCommandLine() { return GetCommandLineA(); }
};

template <>
struct CmdLineTraits<wchar_t> {
  static constexpr wchar_t Space = L' ';
  static constexpr wchar_t Tab = L'\t';
  static constexpr wchar_t Quote = L'"';
  static CharT* GetCommandLine() { return GetCommandLineW(); }
};

template <typename CharT>
static CharT* getWinMainCmdLineImpl() {
  using Traits = CmdLineTraits<CharT>;

  CharT* cmdline = Traits::GetCommandLine();
  bool inQuotes = false;

  // Skip program name (first argument)
  while (*cmdline) {
    if (*cmdline == Traits::Quote)
      inQuotes = !inQuotes;
    else if ((*cmdline == Traits::Space || *cmdline == Traits::Tab) && !inQuotes)
      break;
    ++cmdline;
  }

  // Skip whitespace after program name
  while (*cmdline == Traits::Space || *cmdline == Traits::Tab)
    ++cmdline;

  return cmdline;
}

} // namespace

//===----------------------------------------------------------------------===//
// Public API (crt namespace)
//===----------------------------------------------------------------------===//

namespace crt {

void initArgsA() {
  // Call UCRT to parse command line and set up __argc, __argv, _environ
  // _DoWildCard=0: Don't expand wildcards (matches typical CRT behavior)
  int result = __getmainargs(&Argc, &Argv, &Environ, 0, &StartupInfo);
  if (result != 0) {
    fatalError(RuntimeError::SpaceArg);
  }
}

void initArgsW() {
  // Wide character version
  int result = __wgetmainargs(&Argc, &Wargv, &Wenviron, 0, &StartupInfo);
  if (result != 0) {
    fatalError(RuntimeError::SpaceArg);
  }
}

void initEnvironA() {
  // Environment is already initialized by __getmainargs, but if called
  // separately (e.g., for DLLs), we need to ensure Environ is set
  if (!Environ) {
    char** dummy_argv;
    __getmainargs(&Argc, &dummy_argv, &Environ, 0, &StartupInfo);
  }
}

void initEnvironW() {
  // Wide environment - same as above
  if (!Wenviron) {
    wchar_t** dummy_wargv;
    __wgetmainargs(&Argc, &dummy_wargv, &Wenviron, 0, &StartupInfo);
  }
}

LPSTR getWinMainCmdLineA() { return getWinMainCmdLineImpl<char>(); }

LPWSTR getWinMainCmdLineW() { return getWinMainCmdLineImpl<wchar_t>(); }

int getArgc() { return Argc; }
char** getArgv() { return Argv; }
wchar_t** getWargv() { return Wargv; }
char** getEnviron() { return Environ; }
wchar_t** getWenviron() { return Wenviron; }

} // namespace crt

#endif // _WIN32
