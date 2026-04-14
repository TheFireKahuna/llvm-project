//===-- Windows implementation of uname ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Populates struct utsname from KUSER_SHARED_DATA (true OS version, immune
// to PEB version spoofing) and internal::gethostname (registry-backed,
// cached). The UBR (Update Build Revision) is read from the registry to
// provide the full four-part version in release. No Win32 API calls.
//
//===----------------------------------------------------------------------===//

#include "src/sys/utsname/uname.h"

#include "hdr/errno_macros.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/io/misc_ops.h"
#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_api.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#include <sys/utsname.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

// Query the Update Build Revision (UBR) from the registry.
// Returns 0 on failure (UBR simply omitted from the release string).
unsigned int query_ubr() {
  WCHAR key_path[] = u"\\Registry\\Machine\\SOFTWARE"
                     u"\\Microsoft\\Windows NT\\CurrentVersion";
  UNICODE_STRING key_name;
  key_name.Buffer = key_path;
  key_name.Length = sizeof(key_path) - sizeof(WCHAR);
  key_name.MaximumLength = sizeof(key_path);

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = nullptr;
  oa.ObjectName = &key_name;
  oa.Attributes = OBJ_CASE_INSENSITIVE;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  HANDLE key;
  NTSTATUS status = ::NtOpenKeyEx(&key, KEY_QUERY_VALUE, &oa, 0);
  if (!NT_SUCCESS(status))
    return 0;

  WCHAR value_name_buf[] = u"UBR";
  UNICODE_STRING value_name;
  value_name.Buffer = value_name_buf;
  value_name.Length = sizeof(value_name_buf) - sizeof(WCHAR);
  value_name.MaximumLength = sizeof(value_name_buf);

  // Buffer for KEY_VALUE_PARTIAL_INFORMATION header + one DWORD.
  alignas(KEY_VALUE_PARTIAL_INFORMATION) char
      buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
  ULONG result_len = 0;

  status = ::NtQueryValueKey(key, &value_name, KeyValuePartialInformation, buf,
                             sizeof(buf), &result_len);
  ::NtClose(key);

  if (!NT_SUCCESS(status))
    return 0;

  auto *info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION *>(buf);
  if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG))
    return 0;

  ULONG ubr;
  __builtin_memcpy(&ubr, info->Data, sizeof(ULONG));
  return ubr;
}

} // namespace

LLVM_LIBC_FUNCTION(int, uname, (struct utsname * name)) {
  if (!name) {
    libc_errno = EFAULT;
    return -1;
  }

  // Zero the entire struct to prevent info leaks from padding bytes and
  // simplify null-termination on all paths.
  __builtin_memset(name, 0, sizeof(*name));

  // KUSER_SHARED_DATA is mapped read-only at 0x7FFE0000 on all NT versions.
  // Use volatile to prevent the compiler from reordering or eliding reads,
  // consistent with other KUSER_SHARED_DATA access in this codebase.
  const volatile auto *kusd =
      reinterpret_cast<const volatile KUSER_SHARED_DATA *>(0x7FFE0000);

  // Snapshot all fields into locals in a tight sequence. The version fields
  // are immutable after boot so there is no true race, but snapshotting
  // ensures consistency if this pattern is ever extended to mutable fields.
  const unsigned int major = kusd->NtMajorVersion;
  const unsigned int minor = kusd->NtMinorVersion;
  // Mask off the upper nibble: on checked/debug builds of Windows, bit 28+
  // encode the build type flag. RtlGetVersion applies the same mask.
  const unsigned int build = kusd->NtBuildNumber & 0x0FFFFFFF;
  const unsigned int ptype = kusd->NtProductType;

  // sysname: identify as NTPOSIX (distinct from "Windows" to reflect the
  // POSIX-on-NT environment, matching __NTPOSIX__ define).
  __builtin_memcpy(name->sysname, "NTPOSIX", sizeof("NTPOSIX"));

  // nodename: hostname from Tcpip\Parameters registry key (cached).
  if (internal::gethostname(name->nodename, sizeof(name->nodename)) < 0)
    __builtin_memcpy(name->nodename, "localhost", sizeof("localhost"));

  // release: "MAJOR.MINOR.BUILD.UBR" (e.g., "10.0.26100.3915").
  // Always four-part for consistent parsing. UBR defaults to 0 if the
  // registry read fails (should not happen on Windows 11+).
  {
    unsigned int ubr = query_ubr();
    cpp::StringStream ss(name->release);
    ss << major << '.' << minor << '.' << build << '.' << ubr
       << cpp::StringStream::ENDS;
  }

  // version: "#BUILD NtProductType" (matches uname -v convention of
  // providing build metadata).
  {
    const char *ptstr = "Unknown";
    switch (ptype) {
    case 1:
      ptstr = "WinNT";
      break; // NtProductWinNt (workstation)
    case 2:
      ptstr = "LanManNt";
      break; // NtProductLanManNt (domain controller)
    case 3:
      ptstr = "Server";
      break; // NtProductServer
    }
    cpp::StringStream(name->version)
        << '#' << build << ' ' << ptstr << cpp::StringStream::ENDS;
  }

  // machine: compile-time architecture.
  {
    constexpr const char arch[] =
#if defined(__x86_64__) || defined(_M_X64)
        "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
        "i686";
#elif defined(__arm__) || defined(_M_ARM)
        "armv7l";
#else
        "unknown";
#endif
    __builtin_memcpy(name->machine, arch, sizeof(arch));
  }

  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
