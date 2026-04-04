//===- Win32/Process.cpp - Win32 Process Implementation ------- -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides the Win32 specific implementation of the Process class.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_POSIX
// LLVM libc provides POSIX APIs on Windows. For process-level queries beyond
// POSIX scope (command line, commit charge), we call ntdll directly.
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/Windows/WindowsSupport.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

// --- NT declarations for features beyond POSIX scope ---
// Struct types (VM_COUNTERS, UNICODE_STRING) come from <sys/ntabi.h> via
// WindowsSupport.h.
extern "C" {
__declspec(dllimport) NTSTATUS NTAPI NtQueryInformationProcess(
    HANDLE, ULONG, void *, ULONG, ULONG *);
} // extern "C"

static const auto NtCurrentProcess_ = reinterpret_cast<HANDLE>(
    static_cast<LONG_PTR>(-1));

// Process information classes not yet in <sys/ntabi.h>.
static constexpr ULONG ProcessVmCounters_ = 3;
static constexpr ULONG ProcessCommandLineInfo_ = 60;

using namespace llvm;

Process::Pid Process::getProcessId() {
  static_assert(sizeof(Pid) >= sizeof(pid_t),
                "Process::Pid should be big enough to store pid_t");
  return Pid(::getpid());
}

Expected<unsigned> Process::getPageSize() {
  static long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size == -1)
    return errorCodeToError(errnoAsErrorCode());
  return static_cast<unsigned>(page_size);
}

size_t Process::GetMallocUsage() {
  // Query current private commit charge via NT — the closest equivalent to
  // the Win32 _heapwalk approach. PagefileUsage is the total committed
  // private memory (heap + stack + other private mappings).
  VM_COUNTERS VMC;
  NTSTATUS St = NtQueryInformationProcess(NtCurrentProcess_,
      ProcessVmCounters_, &VMC, sizeof(VMC), nullptr);
  if (St >= 0)
    return VMC.PagefileUsage;
  return 0;
}

void Process::GetTimeUsage(TimePoint<> &elapsed,
                           std::chrono::nanoseconds &user_time,
                           std::chrono::nanoseconds &sys_time) {
  elapsed = std::chrono::system_clock::now();
  struct rusage RU;
  if (::getrusage(RUSAGE_SELF, &RU) == 0) {
    user_time = std::chrono::seconds(RU.ru_utime.tv_sec) +
                std::chrono::microseconds(RU.ru_utime.tv_usec);
    sys_time = std::chrono::seconds(RU.ru_stime.tv_sec) +
               std::chrono::microseconds(RU.ru_stime.tv_usec);
  }
}

void Process::PreventCoreFiles() {
  struct rlimit rlim;
  getrlimit(RLIMIT_CORE, &rlim);
  rlim.rlim_cur = 0;
  setrlimit(RLIMIT_CORE, &rlim);
  coreFilesPrevented = true;
}

std::optional<std::string> Process::GetEnv(StringRef Name) {
  std::string NameStr = Name.str();
  const char *Val = ::getenv(NameStr.c_str());
  if (!Val)
    return std::nullopt;
  return std::string(Val);
}

std::error_code Process::FixupStandardFileDescriptors() {
  return std::error_code();
}

std::error_code Process::SafelyCloseFileDescriptor(int FD) {
  if (::close(FD) < 0)
    return errnoAsErrorCode();
  return std::error_code();
}

bool Process::StandardInIsUserInput() { return FileDescriptorIsDisplayed(0); }
bool Process::StandardOutIsDisplayed() { return FileDescriptorIsDisplayed(1); }
bool Process::StandardErrIsDisplayed() { return FileDescriptorIsDisplayed(2); }

bool Process::FileDescriptorIsDisplayed(int fd) { return isatty(fd); }

static unsigned getColumnsFromEnv() {
  if (const char *Cols = std::getenv("COLUMNS")) {
    int Val = std::atoi(Cols);
    if (Val > 0)
      return static_cast<unsigned>(Val);
  }
  return 0;
}

unsigned Process::StandardOutColumns() {
  if (!StandardOutIsDisplayed())
    return 0;
  return getColumnsFromEnv();
}

unsigned Process::StandardErrColumns() {
  if (!StandardErrIsDisplayed())
    return 0;
  return getColumnsFromEnv();
}

bool Process::FileDescriptorHasColors(int fd) {
  // Windows Terminal and ConHost on Windows 11+ support ANSI escape codes
  // natively. If the fd is a terminal, assume color support — this matches
  // the Win32 path's behavior (which always returns true for console handles).
  return FileDescriptorIsDisplayed(fd);
}

bool Process::StandardOutHasColors() { return FileDescriptorHasColors(1); }
bool Process::StandardErrHasColors() { return FileDescriptorHasColors(2); }

void Process::UseANSIEscapeCodes(bool /*enable*/) {}
bool Process::ColorNeedsFlush() { return false; }

const char *Process::OutputColor(char code, bool bold, bool bg) {
  return colorcodes[bg ? 1 : 0][bold ? 1 : 0][code & 15];
}

const char *Process::OutputBold(bool bg) { return "\033[1m"; }
const char *Process::OutputReverse() { return "\033[7m"; }
const char *Process::ResetColor() { return "\033[0m"; }

unsigned Process::GetRandomNumber() {
  // Our libc provides getentropy() backed by ProcessPrng (bcrypt).
  unsigned Ret;
  if (::getentropy(&Ret, sizeof(Ret)) == 0)
    return Ret;
  // Fallback (should not happen — getentropy always succeeds on Windows).
  static int x = (static_cast<void>(::srand(
      hash_combine(std::chrono::high_resolution_clock::now()
                       .time_since_epoch()
                       .count(),
                   ::getpid()))), 0);
  (void)x;
  return ::rand();
}

[[noreturn]] void Process::ExitNoCleanup(int RetCode) { _Exit(RetCode); }

// Windows-specific functions still needed by other code.
// Under LLVM libc we provide minimal implementations.
llvm::VersionTuple llvm::GetWindowsOSVersion() {
  // Report Windows 11 (10.0.0.22000) as a reasonable default.
  return llvm::VersionTuple(10, 0, 0, 22000);
}

bool llvm::RunningWindows8OrGreater() { return true; }
bool llvm::RunningWindows11OrGreater() { return true; }

bool llvm::MakeErrMsg(std::string *ErrMsg, const std::string &prefix) {
  if (!ErrMsg)
    return true;
  *ErrMsg = prefix + ": " + std::string(strerror(errno));
  return true;
}

namespace llvm::sys::windows {

std::error_code
GetCommandLineArguments(SmallVectorImpl<const char *> &Args,
                        BumpPtrAllocator &Alloc) {
  // Query the process command line from NT — POSIX uses main(argc,argv)
  // directly, but some LLVM tools call this for wildcard expansion.
  ULONG BufLen = 1024;
  auto *Buf = static_cast<uint8_t *>(std::malloc(BufLen));
  if (!Buf)
    return std::make_error_code(std::errc::not_enough_memory);

  NTSTATUS St = NtQueryInformationProcess(NtCurrentProcess_,
      ProcessCommandLineInfo_, Buf, BufLen, &BufLen);
  if (St == static_cast<NTSTATUS>(0xC0000004L) /*STATUS_INFO_LENGTH_MISMATCH*/) {
    auto *NewBuf = static_cast<uint8_t *>(std::realloc(Buf, BufLen));
    if (!NewBuf) {
      std::free(Buf);
      return std::make_error_code(std::errc::not_enough_memory);
    }
    Buf = NewBuf;
    St = NtQueryInformationProcess(NtCurrentProcess_,
        ProcessCommandLineInfo_, Buf, BufLen, &BufLen);
  }
  if (St < 0) {
    std::free(Buf);
    return std::make_error_code(std::errc::not_supported);
  }

  // Result is a UNICODE_STRING (header + data) within the output buffer.
  auto *CmdLine = reinterpret_cast<UNICODE_STRING *>(Buf);
  size_t Len16 = CmdLine->Length / sizeof(wchar_t);

  // Convert UTF-16 → UTF-8 via LLVM's ConvertUTF (wcstombs fails in C locale).
  auto *Buf16 = reinterpret_cast<const UTF16 *>(CmdLine->Buffer);
  std::string CmdUtf8Str;
  if (!convertUTF16ToUTF8String(ArrayRef<UTF16>(Buf16, Len16), CmdUtf8Str)) {
    std::free(Buf);
    return std::make_error_code(std::errc::illegal_byte_sequence);
  }
  std::free(Buf);
  StringRef CmdUtf8(CmdUtf8Str);

  // Parse using LLVM's Windows command line tokenizer.
  StringSaver Saver(Alloc);
  cl::TokenizeWindowsCommandLineFull(CmdUtf8, Saver, Args,
                                     /*MarkEOLs=*/false);

  if (Args.empty())
    return std::make_error_code(std::errc::invalid_argument);

  return std::error_code();
}

std::error_code widenPath(const Twine &Path8, SmallVectorImpl<wchar_t> &Path16,
                          size_t MaxPathLen) {
  // UTF-8 → UTF-16 via LLVM's ConvertUTF.
  SmallString<256> PathStr;
  Path8.toVector(PathStr);

  SmallVector<UTF16, 256> Utf16;
  if (!convertUTF8ToUTF16String(PathStr, Utf16))
    return std::make_error_code(std::errc::illegal_byte_sequence);
  if (Utf16.size() >= MaxPathLen)
    return std::make_error_code(std::errc::filename_too_long);

  Path16.clear();
  Path16.append(reinterpret_cast<const wchar_t *>(Utf16.data()),
                reinterpret_cast<const wchar_t *>(Utf16.data()) + Utf16.size());
  return std::error_code();
}

} // namespace llvm::sys::windows

#else // !LLVM_RUNTIME_POSIX

#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/WindowsError.h"
#include <malloc.h>
#include <optional>

// The Windows.h header must be after LLVM and standard headers.
#include "llvm/Support/Windows/WindowsSupport.h"

#include <direct.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>

#if !defined(__MINGW32__)
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#endif

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Win32 specific code
//===          and must not be UNIX code
//===----------------------------------------------------------------------===//

using namespace llvm;

Process::Pid Process::getProcessId() {
  static_assert(sizeof(Pid) >= sizeof(DWORD),
                "Process::Pid should be big enough to store DWORD");
  return Pid(::GetCurrentProcessId());
}

// This function retrieves the page size using GetNativeSystemInfo() and is
// present solely so it can be called once to initialize the self_process member
// below.
static unsigned computePageSize() {
  // GetNativeSystemInfo() provides the physical page size which may differ
  // from GetSystemInfo() in 32-bit applications running under WOW64.
  SYSTEM_INFO info;
  GetNativeSystemInfo(&info);
  // FIXME: FileOffset in MapViewOfFile() should be aligned to not dwPageSize,
  // but dwAllocationGranularity.
  return static_cast<unsigned>(info.dwPageSize);
}

Expected<unsigned> Process::getPageSize() {
  static unsigned Ret = computePageSize();
  return Ret;
}

size_t Process::GetMallocUsage() {
  _HEAPINFO hinfo;
  hinfo._pentry = NULL;

  size_t size = 0;

  while (_heapwalk(&hinfo) == _HEAPOK)
    size += hinfo._size;

  return size;
}

void Process::GetTimeUsage(TimePoint<> &elapsed,
                           std::chrono::nanoseconds &user_time,
                           std::chrono::nanoseconds &sys_time) {
  elapsed = std::chrono::system_clock::now();
  ;

  FILETIME ProcCreate, ProcExit, KernelTime, UserTime;
  if (GetProcessTimes(GetCurrentProcess(), &ProcCreate, &ProcExit, &KernelTime,
                      &UserTime) == 0)
    return;

  user_time = toDuration(UserTime);
  sys_time = toDuration(KernelTime);
}

// Some LLVM programs such as llvm-reduce produce core files as a normal part of
// their operation. To prevent the disk from filling up, this configuration item
// does what's necessary to prevent their generation.
void Process::PreventCoreFiles() {
  // Windows does have the concept of core files, called minidumps.  However,
  // disabling minidumps for a particular application extends past the lifetime
  // of that application, which is the incorrect behavior for this API.
  // Additionally, the APIs require elevated privileges to disable and re-
  // enable minidumps, which makes this untenable. For more information, see
  // WerAddExcludedApplication and WerRemoveExcludedApplication (Vista and
  // later).
  //
  // Windows also has modal pop-up message boxes.  As this method is used by
  // llvm-reduce, preventing these pop-ups is additionally important.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);

  coreFilesPrevented = true;
}

/// Returns the environment variable \arg Name's value as a string encoded in
/// UTF-8. \arg Name is assumed to be in UTF-8 encoding.
std::optional<std::string> Process::GetEnv(StringRef Name) {
  // Convert the argument to UTF-16 to pass it to _wgetenv().
  SmallVector<wchar_t, 128> NameUTF16;
  if (windows::UTF8ToUTF16(Name, NameUTF16))
    return std::nullopt;

  // Environment variable can be encoded in non-UTF8 encoding, and there's no
  // way to know what the encoding is. The only reliable way to look up
  // multibyte environment variable is to use GetEnvironmentVariableW().
  SmallVector<wchar_t, MAX_PATH> Buf;
  size_t Size = MAX_PATH;
  do {
    Buf.resize_for_overwrite(Size);
    SetLastError(NO_ERROR);
    Size = GetEnvironmentVariableW(NameUTF16.data(), Buf.data(), Buf.size());
    if (Size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
      return std::nullopt;

    // Try again with larger buffer.
  } while (Size > Buf.size());
  Buf.truncate(Size);

  // Convert the result from UTF-16 to UTF-8.
  SmallVector<char, MAX_PATH> Res;
  if (windows::UTF16ToUTF8(Buf.data(), Size, Res))
    return std::nullopt;
  return std::string(Res.data());
}

/// Perform wildcard expansion of Arg, or just push it into Args if it doesn't
/// have wildcards or doesn't match any files.
static std::error_code WildcardExpand(StringRef Arg,
                                      SmallVectorImpl<const char *> &Args,
                                      StringSaver &Saver) {
  std::error_code EC;

  // Don't expand Arg if it does not contain any wildcard characters. This is
  // the common case. Also don't wildcard expand /?. Always treat it as an
  // option. Paths that start with \\?\ are absolute paths, and aren't
  // expected to be used with wildcard expressions.
  if (Arg.find_first_of("*?") == StringRef::npos || Arg == "/?" ||
      Arg == "-?" || Arg.starts_with("\\\\?\\")) {
    Args.push_back(Arg.data());
    return EC;
  }

  // Convert back to UTF-16 so we can call FindFirstFileW.
  SmallVector<wchar_t, MAX_PATH> ArgW;
  EC = windows::UTF8ToUTF16(Arg, ArgW);
  if (EC)
    return EC;

  // Search for matching files.
  // FIXME:  This assumes the wildcard is only in the file name and not in the
  // directory portion of the file path.  For example, it doesn't handle
  // "*\foo.c" nor "s?c\bar.cpp".
  WIN32_FIND_DATAW FileData;
  HANDLE FindHandle = FindFirstFileW(ArgW.data(), &FileData);
  if (FindHandle == INVALID_HANDLE_VALUE) {
    Args.push_back(Arg.data());
    return EC;
  }

  // Extract any directory part of the argument.
  SmallString<MAX_PATH> Dir = Arg;
  sys::path::remove_filename(Dir);
  const int DirSize = Dir.size();

  do {
    SmallString<MAX_PATH> FileName;
    EC = windows::UTF16ToUTF8(FileData.cFileName, wcslen(FileData.cFileName),
                              FileName);
    if (EC)
      break;

    // Append FileName to Dir, and remove it afterwards.
    llvm::sys::path::append(Dir, FileName);
    Args.push_back(Saver.save(Dir.str()).data());
    Dir.resize(DirSize);
  } while (FindNextFileW(FindHandle, &FileData));

  FindClose(FindHandle);
  return EC;
}

static std::error_code GetExecutableName(SmallVectorImpl<char> &Filename) {
  // The first argument may contain just the name of the executable (e.g.,
  // "clang") rather than the full path, so swap it with the full path.
  wchar_t ModuleName[MAX_PATH];
  size_t Length = ::GetModuleFileNameW(NULL, ModuleName, MAX_PATH);
  if (Length == 0 || Length == MAX_PATH) {
    return mapWindowsError(GetLastError());
  }

  // If the first argument is a shortened (8.3) name (which is possible even
  // if we got the module name), the driver will have trouble distinguishing it
  // (e.g., clang.exe v. clang++.exe), so expand it now.
  Length = GetLongPathNameW(ModuleName, ModuleName, MAX_PATH);
  if (Length == 0)
    return mapWindowsError(GetLastError());
  if (Length > MAX_PATH) {
    // We're not going to try to deal with paths longer than MAX_PATH, so we'll
    // treat this as an error.  GetLastError() returns ERROR_SUCCESS, which
    // isn't useful, so we'll hardcode an appropriate error value.
    return mapWindowsError(ERROR_INSUFFICIENT_BUFFER);
  }

  std::error_code EC = windows::UTF16ToUTF8(ModuleName, Length, Filename);
  if (EC)
    return EC;

  // Make a copy of the filename since assign makes the StringRef invalid.
  std::string Base = sys::path::filename(Filename.data()).str();
  Filename.assign(Base.begin(), Base.end());
  return std::error_code();
}

std::error_code
windows::GetCommandLineArguments(SmallVectorImpl<const char *> &Args,
                                 BumpPtrAllocator &Alloc) {
  const wchar_t *CmdW = GetCommandLineW();
  assert(CmdW);
  std::error_code EC;
  SmallString<MAX_PATH> Cmd;
  EC = windows::UTF16ToUTF8(CmdW, wcslen(CmdW), Cmd);
  if (EC)
    return EC;

  SmallVector<const char *, 20> TmpArgs;
  StringSaver Saver(Alloc);
  cl::TokenizeWindowsCommandLineFull(Cmd, Saver, TmpArgs, /*MarkEOLs=*/false);

  for (const char *Arg : TmpArgs) {
    EC = WildcardExpand(Arg, Args, Saver);
    if (EC)
      return EC;
  }

  if (Args.size() == 0)
    return std::make_error_code(std::errc::invalid_argument);

  SmallVector<char, MAX_PATH> Arg0(Args[0], Args[0] + strlen(Args[0]));
  SmallVector<char, MAX_PATH> Filename;
  sys::path::remove_filename(Arg0);
  EC = GetExecutableName(Filename);
  if (EC)
    return EC;
  sys::path::make_preferred(Arg0);
  sys::path::append(Arg0, Filename);
  Args[0] = Saver.save(Arg0).data();
  return std::error_code();
}

std::error_code Process::FixupStandardFileDescriptors() {
  return std::error_code();
}

std::error_code Process::SafelyCloseFileDescriptor(int FD) {
  if (::close(FD) < 0)
    return errnoAsErrorCode();
  return std::error_code();
}

bool Process::StandardInIsUserInput() { return FileDescriptorIsDisplayed(0); }

bool Process::StandardOutIsDisplayed() { return FileDescriptorIsDisplayed(1); }

bool Process::StandardErrIsDisplayed() { return FileDescriptorIsDisplayed(2); }

bool Process::FileDescriptorIsDisplayed(int fd) {
  DWORD Mode; // Unused
  return (GetConsoleMode((HANDLE)_get_osfhandle(fd), &Mode) != 0);
}

unsigned Process::StandardOutColumns() {
  unsigned Columns = 0;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
    Columns = csbi.dwSize.X;
  return Columns;
}

unsigned Process::StandardErrColumns() {
  unsigned Columns = 0;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &csbi))
    Columns = csbi.dwSize.X;
  return Columns;
}

// The terminal always has colors.
bool Process::FileDescriptorHasColors(int fd) {
  return FileDescriptorIsDisplayed(fd);
}

bool Process::StandardOutHasColors() { return FileDescriptorHasColors(1); }

bool Process::StandardErrHasColors() { return FileDescriptorHasColors(2); }

static bool UseANSI = false;
void Process::UseANSIEscapeCodes(bool enable) {
#if defined(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
  if (enable) {
    HANDLE Console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD Mode;
    GetConsoleMode(Console, &Mode);
    Mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(Console, Mode);
  }
#endif
  UseANSI = enable;
}

namespace {
class DefaultColors {
private:
  WORD defaultColor;

public:
  DefaultColors() : defaultColor(GetCurrentColor()) {}
  static unsigned GetCurrentColor() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
      return csbi.wAttributes;
    return 0;
  }
  WORD operator()() const { return defaultColor; }
};

DefaultColors defaultColors;

WORD fg_color(WORD color) {
  return color & (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY |
                  FOREGROUND_RED);
}

WORD bg_color(WORD color) {
  return color & (BACKGROUND_BLUE | BACKGROUND_GREEN | BACKGROUND_INTENSITY |
                  BACKGROUND_RED);
}
} // namespace

bool Process::ColorNeedsFlush() { return !UseANSI; }

const char *Process::OutputBold(bool bg) {
  if (UseANSI)
    return "\033[1m";

  WORD colors = DefaultColors::GetCurrentColor();
  if (bg)
    colors |= BACKGROUND_INTENSITY;
  else
    colors |= FOREGROUND_INTENSITY;
  SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), colors);
  return 0;
}

const char *Process::OutputColor(char code, bool bold, bool bg) {
  if (UseANSI)
    return colorcodes[bg ? 1 : 0][bold ? 1 : 0][code & 15];

  WORD current = DefaultColors::GetCurrentColor();
  WORD colors;
  if (bg) {
    colors = ((code & 1) ? BACKGROUND_RED : 0) |
             ((code & 2) ? BACKGROUND_GREEN : 0) |
             ((code & 4) ? BACKGROUND_BLUE : 0);
    if (bold)
      colors |= BACKGROUND_INTENSITY;
    colors |= fg_color(current);
  } else {
    colors = ((code & 1) ? FOREGROUND_RED : 0) |
             ((code & 2) ? FOREGROUND_GREEN : 0) |
             ((code & 4) ? FOREGROUND_BLUE : 0);
    if (bold)
      colors |= FOREGROUND_INTENSITY;
    colors |= bg_color(current);
  }
  SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), colors);
  return 0;
}

static WORD GetConsoleTextAttribute(HANDLE hConsoleOutput) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
  return info.wAttributes;
}

const char *Process::OutputReverse() {
  if (UseANSI)
    return "\033[7m";

  const WORD attributes =
      GetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE));

  const WORD foreground_mask = FOREGROUND_BLUE | FOREGROUND_GREEN |
                               FOREGROUND_RED | FOREGROUND_INTENSITY;
  const WORD background_mask = BACKGROUND_BLUE | BACKGROUND_GREEN |
                               BACKGROUND_RED | BACKGROUND_INTENSITY;
  const WORD color_mask = foreground_mask | background_mask;

  WORD new_attributes =
      ((attributes & FOREGROUND_BLUE) ? BACKGROUND_BLUE : 0) |
      ((attributes & FOREGROUND_GREEN) ? BACKGROUND_GREEN : 0) |
      ((attributes & FOREGROUND_RED) ? BACKGROUND_RED : 0) |
      ((attributes & FOREGROUND_INTENSITY) ? BACKGROUND_INTENSITY : 0) |
      ((attributes & BACKGROUND_BLUE) ? FOREGROUND_BLUE : 0) |
      ((attributes & BACKGROUND_GREEN) ? FOREGROUND_GREEN : 0) |
      ((attributes & BACKGROUND_RED) ? FOREGROUND_RED : 0) |
      ((attributes & BACKGROUND_INTENSITY) ? FOREGROUND_INTENSITY : 0) | 0;
  new_attributes = (attributes & ~color_mask) | (new_attributes & color_mask);

  SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), new_attributes);
  return 0;
}

const char *Process::ResetColor() {
  if (UseANSI)
    return "\033[0m";
  SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), defaultColors());
  return 0;
}

static unsigned GetRandomNumberSeed() {
  // Generate a random number seed from the millisecond-resolution Windows
  // system clock and the current process id.
  FILETIME Time;
  GetSystemTimeAsFileTime(&Time);
  DWORD Pid = GetCurrentProcessId();
  return hash_combine(Time.dwHighDateTime, Time.dwLowDateTime, Pid);
}

static unsigned GetPseudoRandomNumber() {
  // Arrange to call srand once when this function is first used, and
  // otherwise (if GetRandomNumber always succeeds in using
  // CryptGenRandom) don't bother at all.
  static int x = (static_cast<void>(::srand(GetRandomNumberSeed())), 0);
  (void)x;
  return ::rand();
}

unsigned Process::GetRandomNumber() {
  // Try to use CryptGenRandom.
  HCRYPTPROV HCPC;
  if (::CryptAcquireContextW(&HCPC, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT)) {
    ScopedCryptContext CryptoProvider(HCPC);
    unsigned Ret;
    if (::CryptGenRandom(CryptoProvider, sizeof(Ret),
                         reinterpret_cast<BYTE *>(&Ret)))
      return Ret;
  }

  // If that fails, fall back to pseudo-random numbers.
  return GetPseudoRandomNumber();
}

typedef NTSTATUS(WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

static RTL_OSVERSIONINFOEXW GetWindowsVer() {
  auto getVer = []() -> RTL_OSVERSIONINFOEXW {
    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    assert(hMod);

    auto getVer =
        (RtlGetVersionPtr)(void *)::GetProcAddress(hMod, "RtlGetVersion");
    assert(getVer);

    RTL_OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    NTSTATUS r = getVer((PRTL_OSVERSIONINFOW)&info);
    (void)r;
    assert(r == STATUS_SUCCESS);

    return info;
  };
  static RTL_OSVERSIONINFOEXW info = getVer();
  return info;
}

llvm::VersionTuple llvm::GetWindowsOSVersion() {
  RTL_OSVERSIONINFOEXW info = GetWindowsVer();
  return llvm::VersionTuple(info.dwMajorVersion, info.dwMinorVersion, 0,
                            info.dwBuildNumber);
}

bool llvm::RunningWindows8OrGreater() {
  // Windows 8 is version 6.2, service pack 0.
  return GetWindowsOSVersion() >= llvm::VersionTuple(6, 2, 0, 0);
}

bool llvm::RunningWindows11OrGreater() {
  RTL_OSVERSIONINFOEXW info = GetWindowsVer();
  auto ver = llvm::VersionTuple(info.dwMajorVersion, info.dwMinorVersion, 0,
                                info.dwBuildNumber);

  // Windows Server 2022
  if (info.wProductType == VER_NT_SERVER)
    return ver >= llvm::VersionTuple(10, 0, 0, 20348);

  // Windows 11
  return ver >= llvm::VersionTuple(10, 0, 0, 22000);
}

[[noreturn]] void Process::ExitNoCleanup(int RetCode) {
  TerminateProcess(GetCurrentProcess(), RetCode);
  llvm_unreachable("TerminateProcess doesn't return");
}

namespace llvm::sys::windows {
HMODULE loadSystemModuleSecure(LPCWSTR lpModuleName) {
  // Ensure we load indeed a module from system32 path.
  // As per GetModuleHandle documentation:
  // "If lpModuleName does not include a path and there is more than one loaded
  // module with the same base name and extension, you cannot predict which
  // module handle will be returned.". This mitigates
  // https://learn.microsoft.com/en-us/security-updates/securityadvisories/2010/2269637
  SmallVector<wchar_t, MAX_PATH> Buf;
  size_t Size = MAX_PATH;
  do {
    Buf.resize_for_overwrite(Size);
    SetLastError(NO_ERROR);
    Size = ::GetSystemDirectoryW(Buf.data(), Buf.size());
    if (Size == 0)
      return NULL;

    // Try again with larger buffer.
  } while (Size > Buf.size());

  Buf.truncate(Size);
  Buf.push_back(L'\\');
  Buf.append(lpModuleName, lpModuleName + std::wcslen(lpModuleName));
  Buf.push_back(0);

  return ::GetModuleHandleW(Buf.data());
}
} // namespace llvm::sys::windows

#endif // LLVM_RUNTIME_POSIX
