//===-- Process-resident console publication state --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Small process-owned state for explicit console publication.
//
// This is intentionally narrower than the tty policy state: it tracks the
// current console reference handle that libc has published or inherited for
// process creation, so later spawn/exec paths do not need to consult
// KERNELBASE-private runtime state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_PROCESS_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_PROCESS_STATE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

struct ConsoleProcessState {
  cpp::Atomic<uintptr_t> current_reference;
};

static_assert(__is_trivially_destructible(ConsoleProcessState),
              "ConsoleProcessState must stay trivially destructible for the PCB");

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONSOLE_PROCESS_STATE_H
