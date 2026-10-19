//===-- assert.cpp - Assertion failures -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// _assert and _wassert, which every image's entry object wraps with these
// definitions. They report a failed assertion as the Universal CRT's do: to
// standard error in a console program or under _OUT_TO_STDERR, and otherwise
// in a dialog that offers to abort, debug or ignore. The Universal CRT's end
// the program through its own abort and _exit, which the wraps of those
// cannot reach, and show the dialog through user32.dll after reading two app
// model policies through kernel.appcore.dll, which loads msvcrt.dll with it.
// These read the policies from kernelbase.dll, end the program through
// wincrt's abort and _exit, and raise the dialog as a hard error, which the
// session's csrss.exe displays, so that the process loads no user32.dll and
// does not become a GUI process.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <signal.h>
#include <stdio.h>
#include <wchar.h>

// Exported by ntdll; no SDK header declares them.
extern "C" NTSYSAPI NTSTATUS NTAPI NtOpenThreadToken(HANDLE Thread,
                                                     ACCESS_MASK Access,
                                                     BOOLEAN OpenAsSelf,
                                                     PHANDLE Token);
extern "C" NTSYSAPI NTSTATUS NTAPI NtRaiseHardError(NTSTATUS ErrorStatus,
                                                    ULONG NumberOfParameters,
                                                    ULONG UnicodeStringMask,
                                                    PULONG_PTR Parameters,
                                                    ULONG ValidResponseOptions,
                                                    PULONG Response);

namespace {

// A message box raised as a hard error: its parameters are the text and the
// caption, both strings, and the message box style. The override shows it
// whatever error mode the process has set.
constexpr NTSTATUS ServiceNotification = 0x40000018 | 0x10000000;
constexpr ULONG OptionAbortRetryIgnore = 0;
constexpr ULONG ResponseAbort = 2, ResponseIgnore = 4, ResponseRetry = 7;
// The THREADINFOCLASS that sets a thread's impersonation token.
constexpr ULONG ThreadImpersonationToken = 5;

enum class Action { Abort, Debug, Ignore, Fail };

UNICODE_STRING unicodeString(const wchar_t *Text) {
  auto Bytes = static_cast<USHORT>(wcslen(Text) * sizeof(wchar_t));
  return {Bytes, static_cast<USHORT>(Bytes + sizeof(wchar_t)),
          const_cast<wchar_t *>(Text)};
}

// Shows the dialog. A thread that has asked for no critical-error dialogs
// would get none from the kernel, while the Universal CRT's message box
// ignores that error mode, so it is lifted for the call; the session's
// services have no visible desktop, and csrss.exe answers there for the user.
// csrss.exe reads the strings as the calling thread, so an impersonation is
// set aside, as it does not move the Universal CRT's message box either.
Action raiseHardError(const wchar_t *Text, const wchar_t *Caption) {
  UNICODE_STRING TextString = unicodeString(Text);
  UNICODE_STRING CaptionString = unicodeString(Caption);
  ULONG_PTR Parameters[] = {
      reinterpret_cast<ULONG_PTR>(&TextString),
      reinterpret_cast<ULONG_PTR>(&CaptionString),
      MB_TASKMODAL | MB_ICONHAND | MB_ABORTRETRYIGNORE | MB_SETFOREGROUND};

  DWORD Session = 0, ErrorMode = GetThreadErrorMode();
  bool LiftErrorMode =
      (ErrorMode & SEM_FAILCRITICALERRORS) &&
      ProcessIdToSessionId(GetCurrentProcessId(), &Session) && Session != 0;
  if (LiftErrorMode)
    SetThreadErrorMode(ErrorMode & ~SEM_FAILCRITICALERRORS, nullptr);
  HANDLE Impersonation = nullptr, NoToken = nullptr;
  if (NT_SUCCESS(NtOpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE,
                                   TRUE, &Impersonation)))
    NtSetInformationThread(GetCurrentThread(),
                           static_cast<THREADINFOCLASS>(
                               ThreadImpersonationToken),
                           &NoToken, sizeof(NoToken));

  ULONG Response = 0;
  NTSTATUS Status =
      NtRaiseHardError(ServiceNotification, 3, 0b11, Parameters,
                       OptionAbortRetryIgnore, &Response);

  if (Impersonation) {
    NtSetInformationThread(GetCurrentThread(),
                           static_cast<THREADINFOCLASS>(
                               ThreadImpersonationToken),
                           &Impersonation, sizeof(Impersonation));
    CloseHandle(Impersonation);
  }
  if (LiftErrorMode)
    SetThreadErrorMode(ErrorMode, nullptr);

  if (!NT_SUCCESS(Status))
    return Action::Fail;
  switch (Response) {
  case ResponseAbort:
    return Action::Abort;
  case ResponseRetry:
    return Action::Debug;
  case ResponseIgnore:
    return Action::Ignore;
  default:
    return Action::Fail;
  }
}

// Decides as the Universal CRT does whether to show the dialog. Under a
// debugger the message goes to the debugger, and a process with no classic
// desktop windows breaks into it; a process whose app model shows no
// developer diagnostics, or has no classic desktop windows, aborts, unless a
// debugger is present. The Universal CRT asks no app model policy in a secure
// process, and takes the defaults.
Action showAssertion(const wchar_t *Text, const wchar_t *Caption) {
  AppPolicyWindowingModel Windowing = AppPolicyWindowingModel_ClassicDesktop;
  AppPolicyShowDeveloperDiagnostic Diagnostic =
      AppPolicyShowDeveloperDiagnostic_ShowUI;
  if (!wincrt::isSecureProcess()) {
    __wincrt_AppPolicyGetWindowingModel(GetCurrentThreadEffectiveToken(),
                                        &Windowing);
    __wincrt_AppPolicyGetShowDeveloperDiagnostic(
        GetCurrentThreadEffectiveToken(), &Diagnostic);
  }
  bool Debugger = IsDebuggerPresent();
  bool Desktop = Windowing == AppPolicyWindowingModel_ClassicDesktop;
  if (Debugger) {
    OutputDebugStringW(Text);
    if (!Desktop)
      return Action::Debug;
  }
  if (Diagnostic != AppPolicyShowDeveloperDiagnostic_ShowUI || !Desktop)
    return Debugger ? Action::Debug : Action::Abort;
  return raiseHardError(Text, Caption);
}

void widen(const char *From, wchar_t *To, int Count) {
  if (!MultiByteToWideChar(CP_ACP, 0, From, -1, To, Count))
    To[0] = L'\0';
}
void widen(const wchar_t *From, wchar_t *To, int Count) {
  wcsncpy_s(To, Count, From, _TRUNCATE);
}

// The dialog names the image that asserted, the source location and the
// expression.
template <typename Char>
void assertToMessageBox(const Char *Expression, const Char *File,
                        unsigned Line, void *ReturnAddress) {
  wchar_t Program[MAX_PATH + 1] = L"<program name unknown>";
  HMODULE Module;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(ReturnAddress), &Module))
    GetModuleFileNameW(Module, Program, MAX_PATH + 1);
  wchar_t WideFile[MAX_PATH + 1], WideExpression[512];
  widen(File, WideFile, MAX_PATH + 1);
  widen(Expression, WideExpression, 512);

  wchar_t Text[1024];
  _snwprintf_s(Text, _countof(Text), _TRUNCATE,
               L"Program: %ls\nFile: %ls\nLine: %u\n\nExpression: %ls\n\n"
               L"(Press Retry to debug the application)",
               Program, WideFile, Line, WideExpression);
  switch (showAssertion(Text, L"Assertion failed!")) {
  case Action::Abort:
    // The user has chosen to end the program, so it ends as with the
    // default action for SIGABRT, without the fault report of abort.
    raise(SIGABRT);
    _exit(3);
  case Action::Debug:
    __debugbreak();
    return;
  case Action::Ignore:
    return;
  case Action::Fail:
    abort();
  }
}

// Writes to the console directly when standard error is one, as the
// Universal CRT does for a wide assertion, and otherwise through stdio.
[[noreturn]] void assertToStderr(const wchar_t *Expression,
                                 const wchar_t *File, unsigned Line) {
  HANDLE Stderr = GetStdHandle(STD_ERROR_HANDLE);
  wchar_t Text[1024];
  DWORD Written;
  if (Stderr && Stderr != INVALID_HANDLE_VALUE &&
      GetFileType(Stderr) == FILE_TYPE_CHAR &&
      _snwprintf_s(Text, _countof(Text), _TRUNCATE,
                   L"Assertion failed: %ls, file %ls, line %u\n", Expression,
                   File, Line) >= 0 &&
      WriteConsoleW(Stderr, Text, static_cast<DWORD>(wcslen(Text)), &Written,
                    nullptr))
    abort();
  fwprintf(stderr, L"Assertion failed: %ls, file %ls, line %u\n", Expression,
           File, Line);
  fflush(stderr);
  abort();
}

[[noreturn]] void assertToStderr(const char *Expression, const char *File,
                                 unsigned Line) {
  fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", Expression,
          File, Line);
  fflush(stderr);
  abort();
}

template <typename Char>
void reportAssertion(const Char *Expression, const Char *File, unsigned Line,
                     void *ReturnAddress) {
  int Mode = _set_error_mode(_REPORT_ERRMODE);
  if (Mode == _OUT_TO_STDERR ||
      (Mode == _OUT_TO_DEFAULT && _query_app_type() == _crt_console_app))
    assertToStderr(Expression, File, Line);
  assertToMessageBox(Expression, File, Line, ReturnAddress);
}

} // namespace

extern "C" {

WINCRT_ATEXIT_API void __cdecl __wrap__assert(const char *Expression,
                                              const char *File,
                                              unsigned Line) {
  reportAssertion(Expression, File, Line, __builtin_return_address(0));
}

WINCRT_ATEXIT_API void __cdecl __wrap__wassert(const wchar_t *Expression,
                                               const wchar_t *File,
                                               unsigned Line) {
  reportAssertion(Expression, File, Line, __builtin_return_address(0));
}

} // extern "C"
