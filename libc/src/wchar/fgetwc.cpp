//===-- Implementation of fgetwc ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/wchar/fgetwc.h"

#include "hdr/types/FILE.h"
#include "hdr/types/wint_t.h"
#include "src/__support/File/file.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/read_wchar.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(wint_t, fgetwc, (::FILE * stream)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);
  f->lock();

  // Set wide orientation on first use (C11 §7.21.2p4).
  f->fwide_unlocked(1);

  wint_t result = internal::read_wchar_unlocked(f);

  f->unlock();
  return result;
}

} // namespace LIBC_NAMESPACE_DECL
