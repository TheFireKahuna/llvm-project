//===-- Link-time FileOps registration via SectionRegistry -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each optional FileKind's ops instance registers itself into the
// `.libcops$M` section via LIBC_REGISTER_FILE_OPS. The $A/$Z bookends
// live in file_ops_table.cpp (LIBC_DEFINE_SECTION_BOOKENDS). Tier B
// init walks the range once, populates a flat FileKind → FileOps*
// dispatch table, and verifies the section is MEM_READ without
// MEM_WRITE.
//
// This is the first consumer of section_registry. See that header for
// the security analysis and the rationale behind the three-letter
// ($A/$M/$Z) layout.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_FILE_OPS_SECTION_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_FILE_OPS_SECTION_H

#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// One link-time record per registered FileKind.
struct FileOpsRegistration {
  FileKind kind;
  const FileOps *ops;
};

// Walks `.libcops$M` once at Tier B init, populates the FileKind →
// FileOps* array read by kind_to_ops(), and verifies the output
// section has MEM_READ set / MEM_WRITE clear. Called exactly from
// fd_table_startup_init().
void init_file_ops_table();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Place a FileOpsRegistration into `.libcops$M`. Use exactly once per
// kind, at file scope (outside any namespace {} block).
//
// `kind_name` is the FileKind enumerator (unqualified, e.g. Epoll) and
// also the uniqueness tag — a duplicate `LIBC_REGISTER_FILE_OPS(Epoll,
// ...)` elsewhere in the link produces a duplicate-symbol error from
// the linker, which is what we want.
//
// Example (from ipc/epoll_ops.cpp):
//   LIBC_REGISTER_FILE_OPS(Epoll, epoll_fd_ops)
#define LIBC_REGISTER_FILE_OPS(kind_name, ops_var)                             \
  LIBC_SECTION_REGISTER(libcops,                                               \
                        ::LIBC_NAMESPACE::internal::FileOpsRegistration,       \
                        kind_name,                                             \
                        {::LIBC_NAMESPACE::internal::FileKind::kind_name,      \
                         &(ops_var)})                                          \
  extern "C" [[gnu::used]] void __libc_ops_anchor_##kind_name(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_FILE_OPS_SECTION_H
