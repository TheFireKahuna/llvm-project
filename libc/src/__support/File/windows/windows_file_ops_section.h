//===-- Link-time WindowsFileOps registration via SectionRegistry *- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each concrete Windows File subclass (WindowsFile, IoRingFile, future
// MemStream/Cookie variants) registers its platform callback table into
// `.libcfio$M` via LIBC_REGISTER_WINDOWS_FILE_OPS. The $A/$Z bookends
// live in windows_file_ops_table.cpp (LIBC_DEFINE_SECTION_BOOKENDS).
// Tier B init walks the range once, populates a flat
// WindowsFileKind -> WindowsFileOps* dispatch table, and verifies the
// section is MEM_READ without MEM_WRITE.
//
// Symmetric with the OFD `.libcops` registry at
// libc/src/__support/OSUtil/windows/fd/file_ops_section.h -- same
// SectionRegistry primitive, same null-handler skip for $A/$Z alignment
// padding, same PE-characteristics audit. See section_registry.h for the
// three-section layout and security analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_WINDOWS_FILE_OPS_SECTION_H
#define LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_WINDOWS_FILE_OPS_SECTION_H

#include "hdr/stdint_proxy.h"
#include "src/__support/File/file.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Dense zero-indexed enumeration of Windows File backing classes. Used
// as a direct index into the flat dispatch array. One enumerator per
// concrete subclass under libc/src/__support/File/windows/.
enum class WindowsFileKind : uint8_t {
  Sync = 0,   // WindowsFile -- synchronous NtReadFile/NtWriteFile
  IoRing = 1, // IoRingFile  -- async via per-thread IoRing
  Count,      // sentinel; must remain last
};

// Platform callback table. Field order and types mirror the five
// function-pointer fields that File::File() stores; the ctor of each
// concrete Windows subclass pulls its ops via kind_to_windows_file_ops()
// and hands the pointers to the File base unchanged. `sync` may be null
// for kinds that have no write-behind to drain.
struct WindowsFileOps {
  ::LIBC_NAMESPACE::File::WriteFunc *write;
  ::LIBC_NAMESPACE::File::ReadFunc *read;
  ::LIBC_NAMESPACE::File::SeekFunc *seek;
  ::LIBC_NAMESPACE::File::CloseFunc *close;
  ::LIBC_NAMESPACE::File::SyncFunc *sync;
};

// One link-time record per registered kind. Lives in `.libcfio$M`.
struct WindowsFileOpsRegistration {
  WindowsFileKind kind;
  const WindowsFileOps *ops;
};

// Walks `.libcfio$M` once, populates the WindowsFileKind -> ops* array
// consulted by kind_to_windows_file_ops(), and verifies the output
// section has MEM_READ set / MEM_WRITE clear. Called from
// fd_table_startup_init() directly after init_file_ops_table().
void init_windows_file_ops_table();

// Pre-populated lookup. Must not be called before
// init_windows_file_ops_table() has run -- LIBC_ASSERT traps on a null
// slot, which only occurs if the owning TU wasn't linked.
const WindowsFileOps *kind_to_windows_file_ops(WindowsFileKind kind);

// File-scope ops constants defined in file.cpp. Exposed here so the
// static stdio stream objects (libc/src/stdio/windows/std{in,out,err}.cpp)
// can pass them to WindowsFile's constexpr ctor at link/static-init time
// without going through the runtime kind_to_windows_file_ops() lookup --
// the std streams are compile-time-stable storage, so their ops binding
// must also resolve at link time.
extern const WindowsFileOps windows_file_sync_ops;
extern const WindowsFileOps windows_file_ioring_ops;

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Emit one WindowsFileOpsRegistration into `.libcfio$M`. Use exactly
// once per kind at file scope. `kind_name` is the WindowsFileKind
// enumerator (unqualified) and doubles as the linker uniqueness tag --
// a duplicate expansion produces a duplicate-symbol error from the
// linker. `ops_var` must be an lvalue of type `const WindowsFileOps`
// (typically a file-scope constant defined alongside the class impl).
//
// Example (libc/src/__support/File/windows/file.cpp):
//   LIBC_REGISTER_WINDOWS_FILE_OPS(Sync, windows_file_sync_ops)
#define LIBC_REGISTER_WINDOWS_FILE_OPS(kind_name, ops_var)                     \
  LIBC_SECTION_REGISTER(                                                       \
      libcfio,                                                                 \
      ::LIBC_NAMESPACE::internal::WindowsFileOpsRegistration,                  \
      kind_name,                                                               \
      {::LIBC_NAMESPACE::internal::WindowsFileKind::kind_name, &(ops_var)})   \
  extern "C" [[gnu::used]] void __libc_fio_anchor_##kind_name(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_FILE_WINDOWS_WINDOWS_FILE_OPS_SECTION_H
