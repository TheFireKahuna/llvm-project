//===-- assert.cpp - _assert and _wassert ---------------------------------===//
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
// model policies through kernel.appcore.dll. These end the program through
// wincrt's abort and _exit, and raise the dialog as a hard error, which the
// session's csrss.exe displays, so that the process loads no user32.dll and
// does not become a GUI process.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

using namespace wincrt;

// Exported by ntdll; no SDK header declares them.
extern "C" {
NTSYSAPI NTSTATUS NTAPI NtOpenThreadToken(HANDLE Thread, ACCESS_MASK Access,
                                          BOOLEAN OpenAsSelf, PHANDLE Token);
NTSYSAPI NTSTATUS NTAPI NtRaiseHardError(
    NTSTATUS ErrorStatus, ULONG NumberOfParameters, ULONG UnicodeStringMask,
    PULONG_PTR Parameters, ULONG ValidResponseOptions, PULONG Response);
}

namespace {

// STATUS_SERVICE_NOTIFICATION with HARDERROR_OVERRIDE_ERRORMODE: a message box
// raised as a hard error, shown whatever error mode the process has set. Its
// parameters are the text and the caption, both strings, and the message box
// style.
constexpr NTSTATUS ServiceNotification = 0x40000018 | 0x10000000;
constexpr ULONG OptionAbortRetryIgnore = 0;
constexpr ULONG ResponseAbort = 2, ResponseIgnore = 4, ResponseRetry = 7;
// The THREADINFOCLASS that sets a thread's impersonation token.
constexpr auto ThreadImpersonationToken = static_cast<THREADINFOCLASS>(5);

UNICODE_STRING unicodeString(const wchar_t *Text) {
  auto Bytes = static_cast<USHORT>(wcslen(Text) * sizeof(wchar_t));
  return {Bytes, static_cast<USHORT>(Bytes + sizeof(wchar_t)),
          const_cast<wchar_t *>(Text)};
}

// Shows the dialog and returns the response, or zero if it cannot be shown.
// A thread that has asked for no critical-error dialogs would get none from
// the kernel, while the Universal CRT's message box ignores that error mode,
// so it is lifted for the call; the session's services have no visible
// desktop, and csrss.exe answers there for the user. csrss.exe reads the
// strings as the calling thread, so an impersonation is set aside, as it does
// not move the Universal CRT's message box either.
ULONG raiseHardError(const wchar_t *Text) {
  UNICODE_STRING TextString = unicodeString(Text);
  UNICODE_STRING CaptionString = unicodeString(L"Assertion failed!");
  ULONG_PTR Parameters[] = {reinterpret_cast<ULONG_PTR>(&TextString),
                            reinterpret_cast<ULONG_PTR>(&CaptionString),
                            MB_TASKMODAL | MB_ICONHAND | MB_ABORTRETRYIGNORE |
                                MB_SETFOREGROUND};

  DWORD Session = 0, ErrorMode = GetThreadErrorMode();
  bool LiftErrorMode = (ErrorMode & SEM_FAILCRITICALERRORS) &&
                       ProcessIdToSessionId(GetCurrentProcessId(), &Session) &&
                       Session != 0;
  if (LiftErrorMode)
    SetThreadErrorMode(ErrorMode & ~SEM_FAILCRITICALERRORS, nullptr);
  HANDLE Impersonation = nullptr, NoToken = nullptr;
  if (NT_SUCCESS(NtOpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE,
                                   &Impersonation)))
    NtSetInformationThread(GetCurrentThread(), ThreadImpersonationToken,
                           &NoToken, sizeof(NoToken));

  ULONG Response = 0;
  if (!NT_SUCCESS(NtRaiseHardError(ServiceNotification, 3, 0b11, Parameters,
                                   OptionAbortRetryIgnore, &Response)))
    Response = 0;

  if (Impersonation) {
    NtSetInformationThread(GetCurrentThread(), ThreadImpersonationToken,
                           &Impersonation, sizeof(Impersonation));
    CloseHandle(Impersonation);
  }
  if (LiftErrorMode)
    SetThreadErrorMode(ErrorMode, nullptr);
  return Response;
}

// Decides as the Universal CRT does whether to show the dialog. Under a
// debugger the message goes to the debugger, and a process with no classic
// desktop windows breaks into it; a process whose app model shows no
// developer diagnostics, or has no classic desktop windows, aborts, unless a
// debugger is present.
ULONG showAssertion(const wchar_t *Text) {
  bool Desktop = appPolicy("AppPolicyGetWindowingModel",
                           AppPolicyWindowingModel_ClassicDesktop) ==
                 AppPolicyWindowingModel_ClassicDesktop;
  bool Debugger = IsDebuggerPresent();
  if (Debugger)
    OutputDebugStringW(Text);
  if (!Desktop || appPolicy("AppPolicyGetShowDeveloperDiagnostic",
                            AppPolicyShowDeveloperDiagnostic_ShowUI) !=
                      AppPolicyShowDeveloperDiagnostic_ShowUI)
    return Debugger ? ResponseRetry : ResponseAbort;
  return raiseHardError(Text);
}

void widen(const char *From, wchar_t *To, int Count) {
  if (!MultiByteToWideChar(CP_ACP, 0, From, -1, To, Count))
    To[0] = L'\0';
}
void widen(const wchar_t *From, wchar_t *To, int Count) {
  wcsncpy_s(To, Count, From, _TRUNCATE);
}

// A report is put together from its parts, without the Universal CRT's
// formatted output, which is inline in its headers: an archive that defined
// those functions would offer them to every program that links it.
const char *text(const char *Narrow, const wchar_t *, const char *) {
  return Narrow;
}
const wchar_t *text(const char *, const wchar_t *Wide, const wchar_t *) {
  return Wide;
}
#define WINCRT_TEXT(Like, Text) text(Text, L##Text, Like)

void toDecimal(unsigned Value, char *Text) { _ultoa_s(Value, Text, 16, 10); }
void toDecimal(unsigned Value, wchar_t *Text) { _ultow_s(Value, Text, 16, 10); }

void put(const char *Text) { fputs(Text, stderr); }

// Writes to the console directly when standard error is one, as the
// Universal CRT does for a wide assertion, and otherwise through stdio.
void put(const wchar_t *Text) {
  HANDLE Stderr = GetStdHandle(STD_ERROR_HANDLE);
  DWORD Written;
  if (GetFileType(Stderr) != FILE_TYPE_CHAR ||
      !WriteConsoleW(Stderr, Text, static_cast<DWORD>(wcslen(Text)), &Written,
                     nullptr))
    fputws(Text, stderr);
}

// The dialog names the image that asserted, the source location and the
// expression.
template <typename Char>
void assertToMessageBox(const Char *Expression, const Char *File, unsigned Line,
                        void *ReturnAddress) {
  wchar_t Program[MAX_PATH + 1] = L"<program name unknown>";
  HMODULE Module;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(ReturnAddress), &Module))
    GetModuleFileNameW(Module, Program, MAX_PATH + 1);
  wchar_t WideFile[MAX_PATH + 1], WideExpression[512], Number[16];
  widen(File, WideFile, MAX_PATH + 1);
  widen(Expression, WideExpression, 512);
  toDecimal(Line, Number);
  const wchar_t *Parts[] = {L"Program: ",
                            Program,
                            L"\nFile: ",
                            WideFile,
                            L"\nLine: ",
                            Number,
                            L"\n\nExpression: ",
                            WideExpression,
                            L"\n\n(Press Retry to debug the application)"};
  wchar_t Text[1024] = L"";
  for (const wchar_t *Part : Parts)
    wcsncat_s(Text, _countof(Text), Part, _TRUNCATE);
  switch (showAssertion(Text)) {
  case ResponseAbort:
    // The user has chosen to end the program, so it ends as with the
    // default action for SIGABRT, without the fault report of abort.
    raise(SIGABRT);
    _exit(3);
  case ResponseRetry:
    __debugbreak();
    return;
  case ResponseIgnore:
    return;
  default:
    abort();
  }
}

template <typename Char>
void reportAssertion(const Char *Expression, const Char *File, unsigned Line,
                     void *ReturnAddress) {
  int Mode = _set_error_mode(_REPORT_ERRMODE);
  if (Mode != _OUT_TO_STDERR &&
      (Mode != _OUT_TO_DEFAULT || _query_app_type() != _crt_console_app))
    return assertToMessageBox(Expression, File, Line, ReturnAddress);
  Char Number[16];
  toDecimal(Line, Number);
  const Char *Parts[] = {WINCRT_TEXT(File, "Assertion failed: "),
                         Expression,
                         WINCRT_TEXT(File, ", file "),
                         File,
                         WINCRT_TEXT(File, ", line "),
                         Number,
                         WINCRT_TEXT(File, "\n")};
  for (const Char *Part : Parts)
    put(Part);
  fflush(stderr);
  abort();
}

} // namespace

extern "C" {

WINCRT_API void __cdecl __wrap__assert(const char *Expression, const char *File,
                                       unsigned Line) {
  reportAssertion(Expression, File, Line, __builtin_return_address(0));
}

WINCRT_API void __cdecl __wrap__wassert(const wchar_t *Expression,
                                        const wchar_t *File, unsigned Line) {
  reportAssertion(Expression, File, Line, __builtin_return_address(0));
}

} // extern "C"
