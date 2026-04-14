//===--- Definition of Windows stderr --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declares the global stderr pointer. The File object is constructed in
// fd_table.entries[2] by init_std_fds() during startup.
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/file.h"
#include "src/__support/macros/config.h"

#include "hdr/types/FILE.h"

namespace LIBC_NAMESPACE_DECL {
File *stderr = nullptr;
} // namespace LIBC_NAMESPACE_DECL

extern "C" {
FILE *stderr = nullptr;
}
