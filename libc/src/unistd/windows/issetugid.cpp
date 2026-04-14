//===-- Windows implementation of issetugid --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/unistd/issetugid.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// BSD issetugid(2): returns 1 if the process was started via a setuid or
// setgid exec, 0 otherwise. We detect this by comparing real vs effective
// IDs — if they differ, a setuid/setgid exec must have occurred.
LLVM_LIBC_FUNCTION(int, issetugid, ()) {
  uid_t real_uid = g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED);
  uid_t eff_uid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);
  gid_t real_gid = g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED);
  gid_t eff_gid = g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED);
  return (real_uid != eff_uid || real_gid != eff_gid) ? 1 : 0;
}

} // namespace LIBC_NAMESPACE_DECL
