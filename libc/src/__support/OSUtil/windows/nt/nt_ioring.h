//===-- NT IoRing (io_uring equivalent) API declarations ----- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Umbrella header — includes both types and API declarations.
// Prefer including nt_ioring_types.h or nt_ioring_api.h directly when only
// one category is needed, to reduce include-graph weight.
//===----------------------------------------------------------------------===//
//
// IoRing is the Windows 11+ equivalent of Linux io_uring — batched async I/O
// via shared-memory submission/completion rings with zero per-operation syscall
// overhead after the initial submit.
//
// Architecture:
//   ┌──────────────┐          ┌──────────────┐
//   │  User mode   │          │  Kernel      │
//   │              │          │              │
//   │  Push SQEs   │──shared──│  Process SQs │
//   │  (no syscall)│  memory  │  Post CQEs   │
//   │              │          │              │
//   │  Pop CQEs    │──shared──│              │
//   │  (no syscall)│  memory  │              │
//   │              │          │              │
//   │  Submit ─────│──syscall─│→ drain SQ    │
//   │  (1 call     │          │  issue IRPs  │
//   │   for N ops) │          │  wait CQEs   │
//   └──────────────┘          └──────────────┘
//
// Flow:
//   1. NtQueryIoRingCapabilities — discover version, max ops, features
//   2. NtCreateIoRing — create ring, maps shared SQ/CQ memory
//   3. Push SQEs into SQ ring (pure userspace, no syscall)
//   4. NtSubmitIoRing — submit + optional wait for completions
//   5. Pop CQEs from CQ ring (pure userspace, no syscall)
//   6. NtSetInformationIoRing — register completion event for IOCP bridge
//   7. NtClose — destroy ring
//
// POSIX mapping:
//   io_uring_setup()     → NtCreateIoRing
//   io_uring_register()  → IORING_OP_REGISTER_FILES / REGISTER_BUFFERS (SQE ops)
//   io_uring_enter()     → NtSubmitIoRing
//   SQE push / CQE pop   → direct shared memory access (zero syscalls)
//   eventfd notification → NtSetInformationIoRing (completion event) +
//                          NtAssociateWaitCompletionPacket (IOCP bridge)
//
// Windows 11 21H2+. Versions: v1 (21H2), v3 (22H2, adds WRITE), v4 (24H2,
// adds scatter/gather).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_H

#include "src/__support/OSUtil/windows/nt/nt_ioring_types.h"
#include "src/__support/OSUtil/windows/nt/nt_ioring_api.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_H
