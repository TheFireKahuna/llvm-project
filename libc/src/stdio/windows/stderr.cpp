//===--- Definition of Windows stderr --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/stderr.h"

#include "hdr/stdio_macros.h"
#include "hdr/types/FILE.h"
#include "src/__support/File/windows/file.h"
#include "src/__support/File/windows/windows_file_ops_section.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Unbuffered — single-byte ungetc backing only.
uint8_t stderr_buffer[1];

[[clang::no_destroy]] StdStreamStorage stderr_storage(
    internal::windows_file_sync_ops,
    /*file_handle=*/nullptr, stderr_buffer, sizeof(stderr_buffer), _IONBF,
    File::ModeFlags(File::OpenMode::APPEND),
    /*file_descriptor=*/2);

LLVM_LIBC_VARIABLE(FILE *, stderr) =
    reinterpret_cast<FILE *>(&stderr_storage.slot.sync);

} // namespace LIBC_NAMESPACE_DECL
