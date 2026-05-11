//===-- nt_pal — umbrella header for the Layer 0 PAL ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One-stop include for the seven `nt_pal/` sub-headers. Callers that
// only need one operation family should include the matching sub-header
// directly; this umbrella exists for the historical "drop-in" path
// (replacing `memory/memory_primitives.h`) and for callsites that span
// multiple families.
//
// Sub-headers re-exported (per `NTPOSIX_MEMORY_ARCHITECTURE_DESIGN` §6.0):
//   * `pal_state.h`    — process cookie, large-pages availability,
//                        partition base table.
//   * `placeholder.h`  — placeholder lifecycle + the
//                        `MEM_WRITE_WATCH`-on-every-private-commit invariant.
//   * `section.h`      — section + view-of-section ops.
//   * `protect.h`      — page protection + offer/reclaim +
//                        working-set eviction.
//   * `lock.h`         — page lock / unlock (mlock / munlock).
//   * `write_watch.h`  — atomic dirty-page query+reset.
//   * `large_pages.h`  — `SeLockMemoryPrivilege` probe.
//   * `query.h`        — VA region query + bulk walk +
//                        working-set-ex query.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_H

#include "src/__support/OSUtil/windows/nt_pal/large_pages.h"
#include "src/__support/OSUtil/windows/nt_pal/lock.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/nt_pal/protect.h"
#include "src/__support/OSUtil/windows/nt_pal/query.h"
#include "src/__support/OSUtil/windows/nt_pal/section.h"
#include "src/__support/OSUtil/windows/nt_pal/write_watch.h"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PAL_NT_PAL_H
