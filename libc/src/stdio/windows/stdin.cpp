//===--- Definition of Windows stdin ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/stdin.h"

#include "hdr/stdio_macros.h"
#include "hdr/types/FILE.h"
#include "src/__support/File/windows/file.h"
#include "src/__support/File/windows/windows_file_ops_section.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

constexpr size_t STDIN_BUFFER_SIZE = 512;
uint8_t stdin_buffer[STDIN_BUFFER_SIZE];

// Constant-initialized via the constexpr StdStreamStorage ctor — the
// object is populated at link time, no dynamic initializer needed.
// [[clang::no_destroy]] suppresses the global-destructor registration
// that the union's user-provided dtor (required because WindowsFile /
// IoRingFile inherit a non-trivial Mutex via File base) would otherwise
// emit. The std streams are process-lifetime; no cleanup needed.
[[clang::no_destroy]] StdStreamStorage stdin_storage(
    internal::windows_file_sync_ops,
    /*file_handle=*/nullptr, stdin_buffer, STDIN_BUFFER_SIZE, _IOFBF,
    File::ModeFlags(File::OpenMode::READ),
    /*file_descriptor=*/0);

LLVM_LIBC_VARIABLE(FILE *, stdin) =
    reinterpret_cast<FILE *>(&stdin_storage.slot.sync);

} // namespace LIBC_NAMESPACE_DECL
