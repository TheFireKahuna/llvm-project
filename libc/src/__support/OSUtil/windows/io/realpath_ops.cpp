//===-- Internal realpath engine implementation ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves a path to its canonical absolute form:
//   1. Open the file with NtOpenFile (validates existence)
//   2. Query the NT object name via NtQueryObject(ObjectNameInformation)
//      → e.g. \Device\HarddiskVolume3\Users\foo\file.txt
//   3. Translate the device path to a DOS path via the mount manager
//      → e.g. C:\Users\foo\file.txt
//   4. Convert wide DOS path to UTF-8
//
// This resolves symlinks, "..", ".", and returns ENOENT for missing files —
// matching POSIX realpath semantics.
//
// Returns intptr_t: positive = pointer to result, negative = -errno.
//
//===----------------------------------------------------------------------===//

#include "realpath_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/device_path.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/nt_wchar_converter.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

// Forward-declare malloc/free — we're inside libc, can't include <stdlib.h>.
extern "C" {
void *malloc(size_t);
void free(void *);
}

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t realpath(const char *__restrict path, char *__restrict resolved_path) {
  using LIBC_NAMESPACE::cpp::string_view;
  if (path == nullptr || path[0] == '\0')
    return -EINVAL;
  string_view sv(path);

  // Convert to NT path and open the file to validate existence and resolve
  // symlinks. FILE_READ_ATTRIBUTES is the minimum access needed.
  auto nt_s = path_scratch();
  if (!nt_s) return -ENOMEM;
  WCHAR *nt_path_buf = nt_s.data();
  auto nt = to_nt_path(sv, nt_path_buf, nt_s.size());
  if (!nt.has_value())
    return -nt.error();
  size_t nt_len = nt.value();

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(nt_path_buf, nt_len);
  init_object_attributes(&oa, &name);

  IO_STATUS_BLOCK iosb = {};
  windows::ScopedNtHandle file_handle;
  NTSTATUS st = ::NtOpenFile(
      file_handle.put(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_SYNCHRONOUS_IO_NONALERT);

  if (!NT_SUCCESS(st)) {
    // Map common NTSTATUS to errno.
    if (st == STATUS_OBJECT_NAME_NOT_FOUND ||
        st == STATUS_OBJECT_PATH_NOT_FOUND)
      return -ENOENT;
    else if (st == STATUS_ACCESS_DENIED)
      return -EACCES;
    else if (st == STATUS_OBJECT_NAME_INVALID)
      return -EINVAL;
    else
      return -EIO;
  }

  // Query the full NT object name (resolves symlinks, junctions, etc.).
  // Buffer: OBJECT_NAME_INFORMATION header + up to MAX_NT_PATH_WCHARS.
  auto name_s = info_scratch<OBJECT_NAME_INFORMATION>();
  if (!name_s)
    return -ENOMEM;
  auto *name_info = reinterpret_cast<OBJECT_NAME_INFORMATION *>(name_s.data());

  st = ::NtQueryObject(file_handle.get(), ObjectNameInformation, name_s.data(),
                       static_cast<ULONG>(name_s.size()), nullptr);

  if (!NT_SUCCESS(st))
    return -EIO;

  // Translate \Device\HarddiskVolumeN\... → C:\...
  auto dos_s = path_scratch();
  if (!dos_s) return -ENOMEM;
  WCHAR *dos_buf = dos_s.data();
  ULONG nt_name_chars = name_info->Name.Length / sizeof(WCHAR);
  size_t dos_len = windows_util::device_path_to_dos(
      name_info->Name.Buffer, nt_name_chars, dos_buf, dos_s.size());

  if (dos_len == 0)
    return -EIO;

  // Convert wide DOS path to UTF-8.
  // Worst case: each WCHAR could expand to 3 UTF-8 bytes + NUL.
  int utf8_bytes = windows::utf16_to_utf8(dos_buf, dos_len, nullptr, 0);
  if (utf8_bytes <= 0)
    return -EILSEQ;

  // Normalize backslashes to forward slashes for POSIX.
  // Do this on the wide string before UTF-8 conversion for efficiency.
  for (size_t i = 0; i < dos_len; ++i) {
    if (dos_buf[i] == u'\\')
      dos_buf[i] = u'/';
  }

  // Allocate or use caller's buffer.
  char *out = resolved_path;
  if (out == nullptr) {
    out = static_cast<char *>(::malloc(utf8_bytes + 1));
    if (out == nullptr)
      return -ENOMEM;
  }

  int written = windows::utf16_to_utf8(dos_buf, dos_len, out,
                                        static_cast<size_t>(utf8_bytes));
  if (written < 0) {
    if (resolved_path == nullptr)
      ::free(out);
    return -EILSEQ;
  }

  out[written] = '\0';
  return reinterpret_cast<intptr_t>(out);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
