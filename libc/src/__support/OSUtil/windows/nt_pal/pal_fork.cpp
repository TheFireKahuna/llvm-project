//===-- nt_pal::pal_fork — Layer 0 PAL fork-reinit ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Re-runs the per-process cookie + SeLockMemoryPrivilege probes after
// `RtlCloneUserProcess`. Registered at priority `kForkPrioPal` (= 36),
// which sits in the post-VA / pre-allocator band.
//
// Order rationale (`libc_fork_registry.h`):
//   30  Crystalline   (must run first)
//   31  VaSubstrate
//   32  MappingTable  — still alive during P1.A; deleted in P1.G
//   33  Scratch
//   34  Pkey
//   35  MmapLock
//   36  Pal           ← this hook
//   50+ Memory follow-up / env / allocator
//
// When MappingTable is deleted in P1.G, `kForkPrioPal` will be
// renumbered to 32 to reclaim that slot.
//
// The actual probe lives in `pal_init.cpp` as `pal_fork_reinit_impl` so
// both paths share one probe routine.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace nt_pal {

extern "C" void pal_fork_reinit_impl();

extern "C" void pal_fork_reinit() { pal_fork_reinit_impl(); }

} // namespace nt_pal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FORK_REINIT(pal,
                          ::LIBC_NAMESPACE::internal::kForkPrioPal,
                          &::LIBC_NAMESPACE::nt_pal::pal_fork_reinit)
