//===--- Definition of Windows stdout --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/stdout.h"

#include "hdr/stdio_macros.h"
#include "hdr/types/FILE.h"
#include "src/__support/File/windows/file.h"
#include "src/__support/File/windows/windows_file_ops_section.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

constexpr size_t STDOUT_BUFFER_SIZE = 1024;
uint8_t stdout_buffer[STDOUT_BUFFER_SIZE];

[[clang::no_destroy]] StdStreamStorage stdout_storage(
    internal::windows_file_sync_ops,
    /*file_handle=*/nullptr, stdout_buffer, STDOUT_BUFFER_SIZE, _IOLBF,
    File::ModeFlags(File::OpenMode::APPEND),
    /*file_descriptor=*/1);

LLVM_LIBC_VARIABLE(FILE *, stdout) =
    reinterpret_cast<FILE *>(&stdout_storage.slot.sync);

} // namespace LIBC_NAMESPACE_DECL
