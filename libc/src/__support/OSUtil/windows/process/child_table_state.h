//===-- Child table state for Windows ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CHILD_TABLE_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CHILD_TABLE_STATE_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace process {

struct ChildEntry;
struct PgidBucket;

struct ChildTableState {
  RawMutex lock;
  ChildEntry *active;
  HANDLE any_child_event;
  PgidBucket *pgid_index;
};

} // namespace process
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CHILD_TABLE_STATE_H
