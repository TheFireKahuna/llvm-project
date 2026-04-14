//===-- Implementation of seekdir -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "seekdir.h"

#include "src/__support/File/dir.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <dirent.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, seekdir, (::DIR * dir, long loc)) {
  auto *d = reinterpret_cast<LIBC_NAMESPACE::Dir *>(dir);
  d->seek(loc);
}

} // namespace LIBC_NAMESPACE_DECL
