//===--- Definition of Windows stdin ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declares the global stdin pointer. The File object is constructed in
// fd_table.entries[0] by init_std_fds() during startup.
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"

#include "hdr/types/FILE.h"

namespace LIBC_NAMESPACE_DECL {
File *stdin = nullptr;
} // namespace LIBC_NAMESPACE_DECL

extern "C" {
FILE *stdin = nullptr;
} // extern "C"
