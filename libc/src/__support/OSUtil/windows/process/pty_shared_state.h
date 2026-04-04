//===-- Shared PTY state for tree-scoped devpts ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_SHARED_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_SHARED_STATE_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/struct_winsize.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

inline constexpr uint32_t PTY_SHARED_STATE_MAGIC = 0x59545450; // 'PTTY'
inline constexpr uint16_t PTY_SHARED_STATE_VERSION = 3;

inline constexpr uint16_t PTY_SHARED_FLAG_GRANTED = 0x0001;
inline constexpr uint16_t PTY_SHARED_FLAG_UNLOCKED = 0x0002;
inline constexpr uint16_t PTY_SHARED_FLAG_HUNGUP = 0x0004;

struct PtySharedState {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint32_t id;

  /// Seqlock counter for lock-free readers. Odd = write in progress,
  /// even = consistent snapshot. Writers increment before and after mutation
  /// under the kernel mutant. Readers spin-retry if the sequence is odd or
  /// changed between their two reads. Aligned uint32_t is atomic on x64.
  uint32_t change_seq;

  uint64_t owner_pid;
  uint64_t owner_create_time;
  uint64_t controller_create_time;
  pid_t controlling_sid;
  pid_t foreground_pgrp;
  pid_t controller_pid;
  uint32_t reserved1;
  struct winsize winsize;
  struct termios attrs;
};

static_assert(__is_trivially_destructible(PtySharedState),
              "PtySharedState must stay POD-like for shared mapping");

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_PTY_SHARED_STATE_H
