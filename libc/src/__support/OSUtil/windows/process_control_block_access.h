//===-- Lightweight PCB read accessors for leaf headers ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declares narrow read/write accessors into the process control block for
// leaf headers that must not include process_control_block.h directly.
//
// This keeps headers such as page_size.h and tls_cleanup.h independent from
// the full PCB type, breaking include cycles with higher-level subsystem
// headers that embed ThreadLifecycle or other heavyweight state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_ACCESS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_ACCESS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup_state.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

uint32_t pcb_page_size();
uint32_t pcb_alloc_granularity();
void *pcb_min_address();
void *pcb_max_address();

TlsCleanupState &pcb_tls_cleanup();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_ACCESS_H
