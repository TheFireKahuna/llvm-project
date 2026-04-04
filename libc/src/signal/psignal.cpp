//===-- Implementation of psignal -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/signal/psignal.h"

#include "src/signal/signal_print_utils.h"
#include "src/__support/StringUtil/signal_to_string.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, psignal, (int signum, const char *message)) {
  internal::SignalTextWriter writer(stderr);
  internal::write_message_prefix(writer, message);
  writer.write_trimmed(get_signal_string(signum));
  writer.write("\n");
  writer.finish();
}

} // namespace LIBC_NAMESPACE_DECL
