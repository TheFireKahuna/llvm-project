//===-- Non-inheritable OBJECT_ATTRIBUTES helpers --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Centralized helpers for creating OBJECT_ATTRIBUTES that explicitly prevent
// handle inheritance.
//
// On NT, handles are inheritable only when OBJ_INHERIT is set in the
// OBJECT_ATTRIBUTES.Attributes field (or when later toggled via
// NtSetInformationObject(ObjectHandleFlagInformation)). Passing nullptr
// for OBJECT_ATTRIBUTES also produces a non-inheritable handle, but this
// is implicit and easy to overlook during review.
//
// These helpers make the intent explicit: internal libc handles (IOCP,
// ALPC ports, timers, events, sections, IoRing, job objects, etc.) must
// never leak into child processes via NtCreateProcessEx with
// PROCESS_CREATE_FLAGS_INHERIT_HANDLES or NtCreateUserProcess with
// PS_ATTRIBUTE_HANDLE_LIST. When every internal handle is explicitly
// non-inheritable, fork_reinit() in the child only needs to recreate
// per-process kernel state — it doesn't need to close stale inherited
// copies.
//
// Usage:
//   auto oa = internal_oa();
//   NtCreateEvent(&h, EVENT_ALL_ACCESS, &oa, ...);
//
//   auto oa = named_internal_oa(&us, root, sd);
//   NtCreateSectionEx(&h, ACCESS, &oa, ...);
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_HANDLE_ATTRIBUTES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_HANDLE_ATTRIBUTES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

/// Return an anonymous, non-inheritable OBJECT_ATTRIBUTES.
///
/// Use for all internal kernel object creation (events, timers, IOCP,
/// sections, mutants, jobs, threads, etc.) where the handle must not
/// be visible in a child process.
LIBC_INLINE OBJECT_ATTRIBUTES internal_oa() {
  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(OBJECT_ATTRIBUTES);
  oa.RootDirectory = nullptr;
  oa.ObjectName = nullptr;
  oa.Attributes = 0; // No OBJ_INHERIT.
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;
  return oa;
}

/// Return a named, non-inheritable OBJECT_ATTRIBUTES.
///
/// OBJ_CASE_INSENSITIVE is always set (standard for NT object namespace
/// paths). OBJ_INHERIT is never set. Use for named sections, named
/// mutants, named ALPC ports, named jobs, etc. that are internal to libc.
/// The nt_wstring_view must outlive the returned OBJECT_ATTRIBUTES.
LIBC_INLINE OBJECT_ATTRIBUTES named_internal_oa(
    nt_wstring_view *name, HANDLE root = nullptr,
    PSECURITY_DESCRIPTOR sd = nullptr) {
  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(OBJECT_ATTRIBUTES);
  oa.RootDirectory = root;
  oa.ObjectName = name->unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE; // No OBJ_INHERIT.
  oa.SecurityDescriptor = sd;
  oa.SecurityQualityOfService = nullptr;
  return oa;
}

/// Return a named OBJECT_ATTRIBUTES with explicit inheritance control.
///
/// Use for ConDrv console handles and other objects that must be
/// conditionally inheritable when passed to child processes. When
/// inherit is false the handle is non-inheritable; when true
/// OBJ_INHERIT is set so NtCreateProcessEx / NtCreateUserProcess
/// will duplicate it into the child's handle table.
/// The nt_wstring_view must outlive the returned OBJECT_ATTRIBUTES.
LIBC_INLINE OBJECT_ATTRIBUTES named_oa(
    nt_wstring_view *name, HANDLE root = nullptr,
    bool inherit = false) {
  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(OBJECT_ATTRIBUTES);
  oa.RootDirectory = root;
  oa.ObjectName = name->unicode_string();
  oa.Attributes = OBJ_CASE_INSENSITIVE | (inherit ? OBJ_INHERIT : 0);
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;
  return oa;
}

/// Return a named, non-inheritable OBJECT_ATTRIBUTES with a security
/// quality-of-service descriptor (for token duplication, impersonation).
LIBC_INLINE OBJECT_ATTRIBUTES internal_oa_with_sqos(
    PSECURITY_QUALITY_OF_SERVICE sqos) {
  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(OBJECT_ATTRIBUTES);
  oa.RootDirectory = nullptr;
  oa.ObjectName = nullptr;
  oa.Attributes = 0; // No OBJ_INHERIT.
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = sqos;
  return oa;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_HANDLE_ATTRIBUTES_H
