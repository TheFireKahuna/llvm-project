//===- not.cpp - The 'not' testing tool -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Usage:
//   not cmd
//     Will return true if cmd doesn't crash and returns false.
//   not --crash cmd
//     Will return true if cmd crashes (e.g. for testing crash reporting).

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#if defined(LLVM_RUNTIME_WIN32)
#include <windows.h>
#endif

#if defined(LLVM_RUNTIME_POSIX)
extern char **environ;
#endif

using namespace llvm;

int main(int argc, const char **argv) {
  bool ExpectCrash = false;
  std::optional<ArrayRef<StringRef>> Env = std::nullopt;
  SmallVector<std::string, 16> EnvStorage;
  SmallVector<StringRef, 16> EnvRefs;

  ++argv;
  --argc;

  if (argc > 0 && StringRef(argv[0]) == "--crash") {
    ++argv;
    --argc;
    ExpectCrash = true;

    // Crash is expected, so disable crash report and symbolization to reduce
    // output and avoid potentially slow symbolization.
#if defined(LLVM_RUNTIME_WIN32)
    SetEnvironmentVariableA("LLVM_DISABLE_CRASH_REPORT", "1");
    SetEnvironmentVariableA("LLVM_DISABLE_SYMBOLIZATION", "1");
#elif defined(LLVM_RUNTIME_POSIX)
    auto UpsertEnv = [&](StringRef Name, StringRef Value) {
      std::string Prefix = Name.str();
      Prefix.push_back('=');
      std::string Entry = Prefix;
      Entry += Value.str();

      for (std::string &Current : EnvStorage) {
        if (StringRef(Current).starts_with(Prefix)) {
          Current = std::move(Entry);
          return;
        }
      }
      EnvStorage.push_back(std::move(Entry));
    };

    for (char **Current = environ; Current && *Current; ++Current)
      EnvStorage.emplace_back(*Current);
    UpsertEnv("LLVM_DISABLE_CRASH_REPORT", "1");
    UpsertEnv("LLVM_DISABLE_SYMBOLIZATION", "1");

    EnvRefs.reserve(EnvStorage.size());
    for (const std::string &Entry : EnvStorage)
      EnvRefs.emplace_back(Entry);
    Env = ArrayRef<StringRef>(EnvRefs);
#else
    setenv("LLVM_DISABLE_CRASH_REPORT", "1", 0);
    setenv("LLVM_DISABLE_SYMBOLIZATION", "1", 0);
#endif
    // Try to disable coredumps for expected crashes as well since this can
    // noticeably slow down running the test suite.
    sys::Process::PreventCoreFiles();
  }

  if (argc == 0)
    return 1;

  auto Program = sys::findProgramByName(argv[0]);
  if (!Program) {
    WithColor::error() << "unable to find `" << argv[0]
                       << "' in PATH: " << Program.getError().message() << "\n";
    return 1;
  }

  SmallVector<StringRef> Argv(ArrayRef(argv, argc));
  std::string ErrMsg;
  int Result = sys::ExecuteAndWait(*Program, Argv, Env, {}, 0, 0, &ErrMsg);
#ifdef LLVM_RUNTIME_WIN32
  // Handle abort() in msvcrt -- It has exit code as 3.  abort(), aka
  // unreachable, should be recognized as a crash.  However, some binaries use
  // exit code 3 on non-crash failure paths, so only do this if we expect a
  // crash.
  if (ExpectCrash && Result == 3)
    Result = -3;
#endif
  if (Result < 0) {
    WithColor::error() << ErrMsg << "\n";
    if (ExpectCrash)
      return 0;
    return 1;
  }

  if (ExpectCrash)
    return 1;

  return Result == 0;
}
