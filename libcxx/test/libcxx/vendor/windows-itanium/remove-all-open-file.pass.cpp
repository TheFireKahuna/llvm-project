//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: target={{.+}}-windows-itanium
// UNSUPPORTED: c++03, c++11, c++14, no-filesystem

// remove_all removes a file's name at once, as POSIX unlink does, even while
// another handle holds the file open, so the directories above it go too.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <windows.h>

namespace fs = std::filesystem;

int main(int, char**) {
  fs::path dir  = fs::temp_directory_path() / "remove-all-open-file.dir";
  fs::path file = dir / "sub" / "held";
  fs::remove_all(dir);
  fs::create_directories(dir / "sub");
  std::FILE* f = std::fopen(file.string().c_str(), "w");
  assert(f);
  std::fclose(f);

  HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  assert(held != INVALID_HANDLE_VALUE);
  std::error_code ec;
  assert(fs::remove_all(dir, ec) == 3);
  assert(!ec);
  assert(!fs::exists(dir));
  CloseHandle(held);
  return 0;
}
