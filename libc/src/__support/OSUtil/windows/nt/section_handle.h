//===-- RAII section object handle ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SectionHandle owns an NT section object handle. Move-only RAII.
//
// Three provenance modes:
//   - Owned (default): destructor calls NtClose.
//   - Adopted: adopt() takes ownership of an existing raw HANDLE.
//   - Borrowed: borrow() creates a non-owning reference (destructor is no-op).
//
// Borrowed handles are needed for fd-cached sections: the fd table owns the
// canonical handle, individual mmap operations borrow it. Making every
// borrow duplicate would be wasteful and break the caching contract.
//
// Factory methods return SectionHandle directly (empty = failure). Callers
// that need the NTSTATUS pass a non-null NTSTATUS* output parameter.
//
// This type lives at the NT layer (nt/) because it wraps a kernel object
// handle — it has no dependency on the memory subsystem, mapping table,
// or placeholder API.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECTION_HANDLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECTION_HANDLE_H

#include "src/__support/OSUtil/windows/memory/view_spec.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_api.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_helpers.h"
#include "src/__support/OSUtil/windows/nt/nt_memory_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

class SectionHandle {
  HANDLE handle_ = nullptr;
  bool owned_ = true;

public:
  // -- Lifecycle --

  LIBC_INLINE SectionHandle() = default;

  LIBC_INLINE ~SectionHandle() {
    if (handle_ && owned_)
      ::NtClose(handle_);
  }

  LIBC_INLINE SectionHandle(SectionHandle &&o) noexcept
      : handle_(o.handle_), owned_(o.owned_) {
    o.handle_ = nullptr;
  }

  LIBC_INLINE SectionHandle &operator=(SectionHandle &&o) noexcept {
    if (this != &o) {
      if (handle_ && owned_)
        ::NtClose(handle_);
      handle_ = o.handle_;
      owned_ = o.owned_;
      o.handle_ = nullptr;
    }
    return *this;
  }

  SectionHandle(const SectionHandle &) = delete;
  SectionHandle &operator=(const SectionHandle &) = delete;

  // -- Observers --

  LIBC_INLINE explicit operator bool() const { return handle_ != nullptr; }
  LIBC_INLINE HANDLE get() const { return handle_; }
  LIBC_INLINE bool is_owned() const { return owned_; }

  // -- Adoption / Borrowing --

  /// Take ownership of an existing raw HANDLE. Destructor will close it.
  static LIBC_INLINE SectionHandle adopt(HANDLE h) {
    SectionHandle s;
    s.handle_ = h;
    s.owned_ = true;
    return s;
  }

  /// Create a non-owning reference. Destructor is a no-op.
  /// Used for fd-cached section handles where the fd table owns the handle.
  static LIBC_INLINE SectionHandle borrow(HANDLE h) {
    SectionHandle s;
    s.handle_ = h;
    s.owned_ = false;
    return s;
  }

  // -- Transfer --

  /// Release ownership, returning the raw HANDLE. Caller must close it.
  /// Used for transferring to the mapping table (register_mapping_take)
  /// or to the child process (spawn handle inheritance).
  /// Must only be called on owned handles — releasing a borrowed handle
  /// would create a double-close when the real owner closes it.
  LIBC_INLINE HANDLE release() {
    LIBC_ASSERT(owned_ && "release() on a borrowed SectionHandle");
    HANDLE h = handle_;
    handle_ = nullptr;
    return h;
  }

  // -- Creation: Anonymous Pagefile-Backed --

  /// Create anonymous pagefile-backed section with full control over
  /// access rights, page protection, and section flags.
  ///
  /// Returns empty SectionHandle on failure. If st is non-null, receives
  /// the raw NTSTATUS for callers that need error differentiation.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_anon(SIZE_T size, ACCESS_MASK access, DWORD page_prot,
              ULONG sec_flags, NTSTATUS *st = nullptr) {
    HANDLE h = nullptr;
    LARGE_INTEGER sec_size;
    sec_size.QuadPart = static_cast<LONGLONG>(size);
    auto oa = internal_oa();
    NTSTATUS status = ::NtCreateSectionEx(
        &h, access, &oa, &sec_size, page_prot, sec_flags, nullptr,
        nullptr, 0);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(h) : SectionHandle();
  }

  /// Create anonymous section with typical defaults for mmap:
  /// RW access, SECTION_MAP_READ|WRITE|QUERY, SEC_COMMIT.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_anon_rw(SIZE_T size, NTSTATUS *st = nullptr) {
    return create_anon(size,
                       SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                       PAGE_READWRITE, SEC_COMMIT, st);
  }

  /// Create anonymous SEC_RESERVE section (demand-commit via VEH).
  /// Requests execute permissions because mmap callers may mprotect
  /// to any combination after mapping.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_anon_reserve(SIZE_T size, NTSTATUS *st = nullptr) {
    return create_anon(
        size,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
            SECTION_QUERY,
        PAGE_EXECUTE_READWRITE, SEC_RESERVE, st);
  }

  // -- Creation: File-Backed --

  /// Create file-backed section from an open file handle.
  ///
  /// The file handle is not duplicated or stored — the kernel holds its
  /// own reference to the file object once the section is created.
  /// The caller's file handle can be closed independently.
  ///
  /// If sd is non-null, the section is created with the given security
  /// descriptor (e.g., POSIX mode → DACL for shm_open).
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_file(HANDLE file, ACCESS_MASK access, DWORD page_prot,
              ULONG sec_flags, PSECURITY_DESCRIPTOR sd = nullptr,
              NTSTATUS *st = nullptr) {
    HANDLE h = nullptr;
    auto oa = internal_oa();
    if (sd)
      oa.SecurityDescriptor = sd;
    NTSTATUS status = ::NtCreateSectionEx(
        &h, access, &oa, nullptr, page_prot, sec_flags, file,
        nullptr, 0);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(h) : SectionHandle();
  }

  /// Create file-backed section with explicit maximum size.
  /// Used when the section may be extended beyond the current file size.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_file_sized(HANDLE file, SIZE_T max_size, ACCESS_MASK access,
                    DWORD page_prot, ULONG sec_flags,
                    PSECURITY_DESCRIPTOR sd = nullptr,
                    NTSTATUS *st = nullptr) {
    HANDLE h = nullptr;
    LARGE_INTEGER sec_size;
    sec_size.QuadPart = static_cast<LONGLONG>(max_size);
    auto oa = internal_oa();
    if (sd)
      oa.SecurityDescriptor = sd;
    NTSTATUS status = ::NtCreateSectionEx(
        &h, access, &oa, &sec_size, page_prot, sec_flags, file,
        nullptr, 0);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(h) : SectionHandle();
  }

  // -- Creation: Named --

  /// Create or open a named section. If the name already exists and the
  /// caller has access, returns a handle to the existing section.
  ///
  /// Used by signal_mailbox, fifo_channel, and shm_open.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  create_named(const UNICODE_STRING *name, SIZE_T size,
               ACCESS_MASK access, DWORD page_prot, ULONG sec_flags,
               PSECURITY_DESCRIPTOR sd = nullptr,
               NTSTATUS *st = nullptr) {
    auto oa = named_internal_oa(name, nullptr, sd);

    HANDLE h = nullptr;
    LARGE_INTEGER sec_size;
    sec_size.QuadPart = static_cast<LONGLONG>(size);
    NTSTATUS status = ::NtCreateSectionEx(
        &h, access, &oa, &sec_size, page_prot, sec_flags, nullptr,
        nullptr, 0);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(h) : SectionHandle();
  }

  /// Open an existing named section by name.
  [[nodiscard]] LIBC_INLINE static SectionHandle
  open_named(const UNICODE_STRING *name, ACCESS_MASK access,
             NTSTATUS *st = nullptr) {
    auto oa = named_internal_oa(name);

    HANDLE h = nullptr;
    NTSTATUS status = ::NtOpenSection(&h, access, &oa);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(h) : SectionHandle();
  }

  // -- Query --

  /// Query the section's maximum size via NtQuerySection.
  /// Returns the size in bytes, or 0 on failure.
  LIBC_INLINE SIZE_T query_size(NTSTATUS *st = nullptr) const {
    SECTION_BASIC_INFORMATION info = {};
    SIZE_T ret_len = 0;
    NTSTATUS status = ::NtQuerySection(
        handle_, SectionBasicInformation, &info, sizeof(info), &ret_len);
    if (st)
      *st = status;
    return NT_SUCCESS(status)
               ? static_cast<SIZE_T>(info.MaximumSize.QuadPart)
               : 0;
  }

  // -- Duplication --

  /// Duplicate this handle for rollback independence, cross-fd sharing, etc.
  /// The duplicate is always owned (will be closed on destruction).
  ///
  /// If access is 0, uses DUPLICATE_SAME_ACCESS (inherits original rights).
  /// Returns empty SectionHandle on failure.
  [[nodiscard]] LIBC_INLINE SectionHandle
  duplicate(ACCESS_MASK access = 0, NTSTATUS *st = nullptr) const {
    HANDLE dup = nullptr;
    ULONG options = (access == 0) ? DUPLICATE_SAME_ACCESS : 0;
    NTSTATUS status = ::NtDuplicateObject(
        NtCurrentProcess(), handle_, NtCurrentProcess(), &dup,
        access, 0, options);
    if (st)
      *st = status;
    return NT_SUCCESS(status) ? SectionHandle::adopt(dup) : SectionHandle();
  }

  // -- Extension --

  /// Grow the section's maximum size in-place. Only valid for pagefile-backed
  /// or writable file-backed sections. The handle is unchanged — no new
  /// handle is created.
  ///
  /// Used by mremap grow-in-place (NtExtendSection on file-backed sections).
  LIBC_INLINE NTSTATUS extend(SIZE_T new_max_size) {
    return nt_helpers::extend_section(handle_, new_max_size);
  }

  // -- ViewSpec construction --

  /// Build a ViewSpec that references this handle (borrowed).
  /// The ViewSpec does not own the handle — this SectionHandle (or the
  /// mapping table, or the fd table) retains ownership.
  LIBC_INLINE ViewSpec view_spec(DWORD prot, LARGE_INTEGER offset = {},
                                  HANDLE file = nullptr,
                                  DWORD flags = 0) const {
    return {handle_, file, offset, prot, flags};
  }
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_SECTION_HANDLE_H
