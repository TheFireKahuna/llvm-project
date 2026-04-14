//===------------- Windows implementation of IO utils -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "io.h"
#include "src/__support/OSUtil/windows/io/read_write.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

void write_to_stderr(cpp::string_view msg) {
  static_cast<void>(internal::write(2, msg.data(), msg.size()));
}

} // namespace LIBC_NAMESPACE_DECL
