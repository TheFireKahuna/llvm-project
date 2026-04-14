//===-- Unified POSIX-to-NT path resolver -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single authority for classifying and resolving POSIX/DOS/UNC paths to NT
// object namespace paths. Handles:
//
//   - /dev/null, /dev/zero, /dev/random, /dev/urandom -> \Device\...
//   - /dev/pts/*, /dev/ptmx, /dev -> PTY virtual filesystem (delegate)
//   - /dev/fd/<n>, /dev/stdin, /dev/stdout, /dev/stderr -> fd aliasing
//   - /dev/tty -> controlling terminal (\Device\ConDrv\Connect)
//   - /tmp/* -> system temp directory
//   - /absolute/path -> system drive root
//   - C:\path, \\server\share, \\?\path, relative -> NT path conversion
//
// All openat/stat/access callers go through this layer to ensure consistent
// path dispatch. The virtual_fs layer is only invoked for PTY paths.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_RESOLVER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_RESOLVER_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/temp_path.h"
#include "src/__support/macros/attributes.h"

// Maximum path length including the \??\ prefix. 32767 is the NT max.
inline constexpr size_t MAX_NT_PATH_WCHARS = 32768;

// Maximum UTF-16 code units in a single path component (NTFS limit).
inline constexpr size_t MAX_COMPONENT_WCHARS = 255;

// ---------------------------------------------------------------------------
// PathKind — what kind of path was passed?
// ---------------------------------------------------------------------------
enum class PathKind : unsigned char {
  Invalid = 0,

  // /dev/* NT device mappings — resolve to \Device\... paths.
  DevNull,
  DevZero,
  DevRandom,
  DevUrandom,

  // /dev/tty — controlling terminal.
  DevTty,

  // /dev/pts/*, /dev/ptmx, /dev, /dev/pts — delegate to virtual_fs.
  DevPty,

  // /dev/fd/<n>, /dev/stdin, /dev/stdout, /dev/stderr — fd aliasing.
  DevFd,

  // /tmp and /tmp/* — mapped to system temp directory.
  PosixTmp,

  // Other POSIX absolute paths — mapped to system drive root.
  PosixAbsolute,

  // DOS absolute (C:\...), drive-relative (C:foo — per-drive CWD),
  // rooted (\foo — current drive root), relative (foo),
  // UNC (\\server\share), verbatim/device (\\?\, \\.\).
  DosAbsolute,
  DosDriveRelative,
  DosRooted,
  DosRelative,
  Unc,
  VerbatimOrDevice,
};

// ---------------------------------------------------------------------------
// ResolvedPath — result of resolve_path().
// ---------------------------------------------------------------------------
struct ResolvedPath {
  PathKind kind;
  int error;      // 0 on success, errno value on failure.
  size_t nt_len;  // WCHARs written to buf (excl NUL). 0 for virtual kinds.
  int virtual_fd; // For DevFd: the source fd number. -1 otherwise.
};

// Internal result used to classify /dev subpaths without losing errno detail.
struct DevSubpathInfo {
  PathKind kind;
  int error; // 0 on success, errno value on failure.
};

// ---------------------------------------------------------------------------
// Wide-string helpers — shared low-level operations on WCHAR buffers.
// ---------------------------------------------------------------------------
namespace path_detail {

// Write the \??\ NT prefix to the start of buf.
LIBC_INLINE void write_nt_prefix(WCHAR *buf) {
  buf[0] = u'\\';
  buf[1] = u'?';
  buf[2] = u'?';
  buf[3] = u'\\';
}

inline constexpr size_t NT_PREFIX_LEN = 4; // u"\\??\\"

// Convert all forward slashes to backslashes in buf[offset..offset+count).
LIBC_INLINE void slashes_to_backslashes(WCHAR *buf, size_t offset,
                                        size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (buf[offset + i] == u'/')
      buf[offset + i] = u'\\';
}

// Result of utf8_to_wide_nt().
struct WideResult {
  size_t chars; // UTF-16 code units written (excl NUL).
  int error;    // 0 on success, errno on failure.
};

// Convert UTF-8 to wide at buf[offset..], convert / to \, NUL-terminate.
// Returns the number of UTF-16 code units written (excl NUL), or error.
LIBC_INLINE WideResult utf8_to_wide_nt(const char *src, size_t src_len,
                                       WCHAR *buf, size_t offset,
                                       size_t max_wchars) {
  if (src_len == 0)
    return {0, 0};
  if (offset >= max_wchars)
    return {0, ENAMETOOLONG};

  ULONG avail = static_cast<ULONG>((max_wchars - offset) * sizeof(WCHAR));
  ULONG wide_bytes = 0;
  NTSTATUS status = ::RtlUTF8ToUnicodeN(buf + offset, avail, &wide_bytes, src,
                                        static_cast<ULONG>(src_len));
  if (!NT_SUCCESS(status))
    return {0, EILSEQ};

  size_t chars = wide_bytes / sizeof(WCHAR);
  if (offset + chars >= max_wchars)
    return {0, ENAMETOOLONG};

  slashes_to_backslashes(buf, offset, chars);
  buf[offset + chars] = u'\0';
  return {chars, 0};
}

// Copy a wide literal into buf. Returns length in WCHARs excl NUL.
LIBC_INLINE size_t write_wide_literal(const WCHAR *literal, size_t literal_len,
                                      WCHAR *buf, size_t max_wchars) {
  if (literal_len >= max_wchars)
    return 0;
  __builtin_memcpy(buf, literal, literal_len * sizeof(WCHAR));
  buf[literal_len] = u'\0';
  return literal_len;
}

// Parse an unsigned decimal integer from a string_view.
// Returns -1 on failure (empty, overflow, non-digit).
LIBC_INLINE int parse_fd_number(LIBC_NAMESPACE::cpp::string_view s) {
  if (s.empty())
    return -1;
  unsigned val = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c < '0' || c > '9')
      return -1;
    if (val > 0x7FFFFFFFu / 10)
      return -1; // pre-multiply overflow check
    val = val * 10 + static_cast<unsigned>(c - '0');
    if (val > 0x7FFFFFFFu)
      return -1; // post-add overflow check
  }
  return static_cast<int>(val);
}

} // namespace path_detail

// ---------------------------------------------------------------------------
// normalize_path_components — resolve '.' and '..' in-place.
//
// Operates on the portion of buf starting at 'start' so the \??\ prefix or
// drive letter is never touched by '..'. Returns the new length in WCHARs,
// or 0 on error (path too deep or component too long).
// ---------------------------------------------------------------------------
LIBC_INLINE size_t normalize_path_components(WCHAR *buf, size_t start,
                                             size_t len) {
  size_t write = start;
  size_t i = start;
  constexpr size_t MAX_DEPTH = 512;
  size_t stack[MAX_DEPTH];
  size_t depth = 0;

  while (i <= len) {
    size_t seg_start = i;
    while (i < len && buf[i] != u'\\')
      ++i;
    size_t seg_len = i - seg_start;

    if (seg_len == 1 && buf[seg_start] == u'.') {
      // '.' — skip (current dir).
    } else if (seg_len == 2 && buf[seg_start] == u'.' &&
               buf[seg_start + 1] == u'.') {
      // '..' — pop previous component.
      if (depth > 0) {
        --depth;
        write = stack[depth];
      }
    } else if (seg_len > 0) {
      if (seg_len > MAX_COMPONENT_WCHARS)
        return 0;
      if (depth >= MAX_DEPTH)
        return 0;
      stack[depth++] = write;
      if (write != seg_start) {
        // Forward copy is safe: write <= seg_start always holds because
        // normalization only compacts (removes . and .. components).
        for (size_t j = 0; j < seg_len; ++j)
          buf[write + j] = buf[seg_start + j];
      }
      write += seg_len;
      if (i < len)
        buf[write++] = u'\\';
    }
    ++i;
  }

  // Remove trailing backslash (unless it's the root, e.g. C:\).
  if (write > start && buf[write - 1] == u'\\' && write > start + 1)
    --write;

  buf[write] = u'\0';
  return write;
}

// ---------------------------------------------------------------------------
// classify_dev_subpath_info — classify /dev/... paths and preserve ENOTDIR
// for known leaf nodes that incorrectly grow trailing components.
// Expects sv to be the full path starting with "/dev".
// ---------------------------------------------------------------------------
LIBC_INLINE DevSubpathInfo
classify_dev_subpath_info(LIBC_NAMESPACE::cpp::string_view sv) {
  using LIBC_NAMESPACE::cpp::string_view;

  // Bare "/dev" or "/dev/"
  if (sv.size() == 4 || (sv.size() == 5 && sv[4] == '/'))
    return {PathKind::DevPty, 0}; // virtual directory

  // Everything past "/dev/" — split into the first component and any tail.
  const string_view name = sv.substr(5);
  const size_t slash = name.find_first_of('/');
  const string_view head =
      slash == string_view::npos ? name : name.substr(0, slash);
  const string_view tail =
      slash == string_view::npos ? string_view() : name.substr(slash + 1);
  const bool has_tail = slash != string_view::npos;

  auto leaf = [has_tail](PathKind kind) -> DevSubpathInfo {
    if (has_tail)
      return {PathKind::Invalid, ENOTDIR};
    return {kind, 0};
  };

  // NT device nodes.
  if (head == "null")
    return leaf(PathKind::DevNull);
  if (head == "zero")
    return leaf(PathKind::DevZero);
  if (head == "random")
    return leaf(PathKind::DevRandom);
  if (head == "urandom")
    return leaf(PathKind::DevUrandom);

  // Controlling terminal.
  if (head == "tty")
    return leaf(PathKind::DevTty);

  // PTY nodes: /dev/ptmx, /dev/pts, /dev/pts/*
  if (head == "ptmx")
    return leaf(PathKind::DevPty);
  if (head == "pts")
    return {PathKind::DevPty, 0};

  // fd aliasing: /dev/fd/<n>, /dev/stdin, /dev/stdout, /dev/stderr
  // Bare "/dev/fd" is a virtual directory (like /dev/pts).
  if (head == "fd") {
    if (!has_tail || tail.empty())
      return {PathKind::DevPty, 0};
    if (tail.find_first_of('/') != string_view::npos)
      return {PathKind::Invalid, ENOTDIR};
    return {PathKind::DevFd, 0};
  }
  if (head == "stdin" || head == "stdout" || head == "stderr")
    return leaf(PathKind::DevFd);

  // Anything else under /dev/ falls through to the filesystem.
  return {PathKind::PosixAbsolute, 0};
}

// ---------------------------------------------------------------------------
// classify_path — cheap prefix-only classification, no allocation.
// ---------------------------------------------------------------------------
LIBC_INLINE PathKind classify_path(const char *path) {
  using LIBC_NAMESPACE::cpp::string_view;

  if (!path || path[0] == '\0')
    return PathKind::Invalid;

  string_view sv(path);

  // POSIX absolute paths.
  if (sv.starts_with('/')) {
    if (sv.starts_with("/dev") && (sv.size() == 4 || sv[4] == '/'))
      return classify_dev_subpath_info(sv).kind;
    if (sv.starts_with("/tmp") && (sv.size() == 4 || sv[4] == '/'))
      return PathKind::PosixTmp;
    return PathKind::PosixAbsolute;
  }

  // Backslash-prefixed paths: UNC (\\server\share), verbatim (\\?\),
  // device (\\.\), or rooted (\foo — current drive root).
  if (sv[0] == '\\') {
    if (sv.size() >= 2 && (sv[1] == '\\' || sv[1] == '/')) {
      if (sv.size() >= 4 && (sv[2] == '?' || sv[2] == '.') &&
          (sv[3] == '\\' || sv[3] == '/'))
        return PathKind::VerbatimOrDevice;
      return PathKind::Unc;
    }
    return PathKind::DosRooted;
  }

  // Drive paths: C:\... (absolute) or C:foo (drive-relative).
  if (sv.size() >= 2 &&
      ((sv[0] >= 'A' && sv[0] <= 'Z') || (sv[0] >= 'a' && sv[0] <= 'z')) &&
      sv[1] == ':') {
    if (sv.size() >= 3 && (sv[2] == '\\' || sv[2] == '/'))
      return PathKind::DosAbsolute;
    return PathKind::DosDriveRelative;
  }

  return PathKind::DosRelative;
}

// ---------------------------------------------------------------------------
// resolve_dev_fd — parse /dev/fd/<n>, /dev/stdin, etc. into an fd number.
// ---------------------------------------------------------------------------
LIBC_INLINE int resolve_dev_fd(const char *path) {
  using LIBC_NAMESPACE::cpp::string_view;
  string_view sv(path);

  if (!sv.starts_with("/dev/"))
    return -1;
  string_view name = sv.substr(5);

  if (name == "stdin")
    return 0;
  if (name == "stdout")
    return 1;
  if (name == "stderr")
    return 2;

  // /dev/fd/<n>
  if (name.starts_with("fd/"))
    return path_detail::parse_fd_number(name.substr(3));

  return -1;
}

// ---------------------------------------------------------------------------
// resolve_dev_device — write the NT device path for /dev/null etc.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult resolve_dev_device(PathKind kind,
                                                       WCHAR *buf,
                                                       size_t max_wchars) {
  static constexpr WCHAR DEV_NULL[] = u"\\Device\\Null";
  static constexpr size_t DEV_NULL_LEN = 12;
  static constexpr WCHAR DEV_CNG[] = u"\\Device\\CNG";
  static constexpr size_t DEV_CNG_LEN = 11;
  static constexpr WCHAR DEV_TTY[] = u"\\Device\\ConDrv\\Connect";
  static constexpr size_t DEV_TTY_LEN = 22;

  size_t len = 0;
  switch (kind) {
  case PathKind::DevNull:
  case PathKind::DevZero:
    len = path_detail::write_wide_literal(DEV_NULL, DEV_NULL_LEN, buf,
                                          max_wchars);
    break;
  case PathKind::DevRandom:
  case PathKind::DevUrandom:
    len = path_detail::write_wide_literal(DEV_CNG, DEV_CNG_LEN, buf,
                                          max_wchars);
    break;
  case PathKind::DevTty:
    len = path_detail::write_wide_literal(DEV_TTY, DEV_TTY_LEN, buf,
                                          max_wchars);
    break;
  default:
    return {0, EINVAL};
  }
  return len ? path_detail::WideResult{len, 0}
             : path_detail::WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_tmp_path — map /tmp/* to the system temp directory.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult resolve_tmp_path(const char *path,
                                                     WCHAR *buf,
                                                     size_t max_wchars) {
  using LIBC_NAMESPACE::cpp::string_view;
  using namespace path_detail;
  string_view sv(path);

  if (!sv.starts_with("/tmp"))
    return {0, EINVAL};
  if (sv.size() > 4 && sv[4] != '/')
    return {0, EINVAL};

  // Resolve the system temp directory as a DOS path with trailing backslash.
  constexpr size_t TEMP_DIR_BUF = 512;
  WCHAR temp_dir[TEMP_DIR_BUF];
  size_t dir_len =
      LIBC_NAMESPACE::windows::get_temp_path_w(temp_dir, TEMP_DIR_BUF);
  if (dir_len == 0)
    return {0, ENOENT};

  // Strip trailing backslash.
  if (dir_len > 0 && temp_dir[dir_len - 1] == u'\\')
    --dir_len;

  // Suffix after "/tmp" (may be empty or "/subpath").
  string_view suffix = sv.substr(4);

  // Build \??\<temp_dir>[\<suffix>]
  size_t suffix_offset = NT_PREFIX_LEN + dir_len;
  auto wr = utf8_to_wide_nt(suffix.data(), suffix.size(), buf, suffix_offset,
                            max_wchars);
  if (wr.error)
    return wr; // propagate EILSEQ / ENAMETOOLONG

  size_t total = NT_PREFIX_LEN + dir_len + wr.chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  write_nt_prefix(buf);
  __builtin_memcpy(buf + NT_PREFIX_LEN, temp_dir, dir_len * sizeof(WCHAR));
  buf[total] = u'\0';

  // Normalize after the drive root. Windows always returns a drive-letter
  // path for %TEMP% (e.g. "C:\Users\...\Temp"), so we skip past "C:\".
  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + 3, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_posix_absolute — map /path to \??\<drive>:\path.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult
resolve_posix_absolute(const char *path, WCHAR *buf, size_t max_wchars) {
  using namespace path_detail;

  if (path[0] != '/')
    return {0, EINVAL};

  const auto *sud = LIBC_NAMESPACE::windows_util::shared_user_data();
  WCHAR drive_letter = sud->NtSystemRoot[0];
  if (drive_letter == u'\0')
    return {0, ENOENT};

  constexpr size_t DRIVE_LEN = 3; // u"C:\\"

  const char *rest = path + 1;
  size_t rest_len = __builtin_strlen(rest);

  auto wr = utf8_to_wide_nt(rest, rest_len, buf, NT_PREFIX_LEN + DRIVE_LEN,
                            max_wchars);
  if (wr.error)
    return wr;

  size_t total = NT_PREFIX_LEN + DRIVE_LEN + wr.chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  // Write \??\C:\ prefix.
  write_nt_prefix(buf);
  buf[4] = drive_letter;
  buf[5] = u':';
  buf[6] = u'\\';
  buf[total] = u'\0';

  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + DRIVE_LEN, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_drive_relative — handle drive-relative paths (C:foo).
// Resolves against the per-drive current directory stored in environment
// variables (=C:, =D:, etc.), falling back to the drive root if unset.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult
resolve_drive_relative(const char *path, WCHAR *buf, size_t max_wchars) {
  using namespace path_detail;

  WCHAR drive_upper = path[0];
  if (drive_upper >= 'a' && drive_upper <= 'z')
    drive_upper -= ('a' - 'A');

  const char *relative = path + 2;
  size_t rel_len = __builtin_strlen(relative);

  // Look up the per-drive CWD from the environment block (=C:).
  WCHAR env_name[3] = {u'=', drive_upper, u':'};
  RTL_USER_PROCESS_PARAMETERS *params = NtCurrentPeb()->ProcessParameters;
  const WCHAR *env_block =
      params ? static_cast<const WCHAR *>(params->Environment) : nullptr;

  WCHAR drive_cwd[512];
  size_t drive_cwd_len = 0;

  if (env_block) {
    size_t val_len = 0;
    const WCHAR *val = LIBC_NAMESPACE::windows::find_env_value_case_insensitive(
        env_block, env_name, 3, &val_len);
    if (val && val_len > 0 && val_len < sizeof(drive_cwd) / sizeof(WCHAR)) {
      __builtin_memcpy(drive_cwd, val, val_len * sizeof(WCHAR));
      drive_cwd[val_len] = u'\0';
      drive_cwd_len = val_len;
    }
  }

  // Fallback: if no per-drive CWD is set, use the drive root (C:\).
  if (drive_cwd_len == 0) {
    drive_cwd[0] = drive_upper;
    drive_cwd[1] = u':';
    drive_cwd[2] = u'\\';
    drive_cwd[3] = u'\0';
    drive_cwd_len = 3;
  }

  // If CWD is the current drive, use the actual CWD.
  UNICODE_STRING *cwd =
      &NtCurrentPeb()->ProcessParameters->CurrentDirectory.DosPath;
  ULONG cwd_chars = cwd->Length / sizeof(WCHAR);
  if (cwd_chars >= 2) {
    WCHAR cwd_drive = cwd->Buffer[0];
    if (cwd_drive >= u'a' && cwd_drive <= u'z')
      cwd_drive -= (u'a' - u'A');
    if (cwd_drive == drive_upper) {
      drive_cwd_len = cwd_chars;
      if (drive_cwd_len >= sizeof(drive_cwd) / sizeof(WCHAR))
        return {0, ENAMETOOLONG};
      __builtin_memcpy(drive_cwd, cwd->Buffer, cwd_chars * sizeof(WCHAR));
      drive_cwd[cwd_chars] = u'\0';
    }
  }

  // Ensure drive CWD ends with backslash.
  if (drive_cwd_len > 0 && drive_cwd[drive_cwd_len - 1] != u'\\') {
    if (drive_cwd_len + 1 >= sizeof(drive_cwd) / sizeof(WCHAR))
      return {0, ENAMETOOLONG};
    drive_cwd[drive_cwd_len++] = u'\\';
    drive_cwd[drive_cwd_len] = u'\0';
  }

  // Convert relative portion to wide at its final position.
  size_t rel_offset = NT_PREFIX_LEN + drive_cwd_len;
  auto wr = utf8_to_wide_nt(relative, rel_len, buf, rel_offset, max_wchars);
  if (wr.error)
    return wr;

  size_t total = NT_PREFIX_LEN + drive_cwd_len + wr.chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  write_nt_prefix(buf);
  __builtin_memcpy(buf + NT_PREFIX_LEN, drive_cwd,
                   drive_cwd_len * sizeof(WCHAR));
  buf[total] = u'\0';

  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + 3, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_rooted_path — handle rooted paths (\foo\bar).
// A rooted path is relative to the root of the current drive.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult
resolve_rooted_path(const char *path, WCHAR *buf, size_t max_wchars) {
  using namespace path_detail;

  UNICODE_STRING *cwd =
      &NtCurrentPeb()->ProcessParameters->CurrentDirectory.DosPath;
  ULONG cwd_chars = cwd->Length / sizeof(WCHAR);
  if (cwd_chars < 2 || cwd->Buffer[1] != u':')
    return {0, ENOENT}; // CWD doesn't have a drive letter.

  WCHAR drive_letter = cwd->Buffer[0];
  constexpr size_t DRIVE_LEN = 2; // u"D:"

  size_t path_len = __builtin_strlen(path);
  auto wr = utf8_to_wide_nt(path, path_len, buf, NT_PREFIX_LEN + DRIVE_LEN,
                            max_wchars);
  if (wr.error)
    return wr;

  size_t total = NT_PREFIX_LEN + DRIVE_LEN + wr.chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  // Write \??\D: prefix (the path already starts with \).
  write_nt_prefix(buf);
  buf[4] = drive_letter;
  buf[5] = u':';
  buf[total] = u'\0';

  // Normalize after D:\ (the path starts with \ at offset DRIVE_LEN).
  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + DRIVE_LEN + 1,
                                          total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_dos_absolute — handle DOS absolute paths (C:\...).
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult
resolve_dos_absolute(const char *path, WCHAR *buf, size_t max_wchars) {
  using namespace path_detail;

  size_t path_len = __builtin_strlen(path);
  auto wr = utf8_to_wide_nt(path, path_len, buf, NT_PREFIX_LEN, max_wchars);
  if (wr.error)
    return wr;

  size_t total = NT_PREFIX_LEN + wr.chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  write_nt_prefix(buf);
  buf[total] = u'\0';
  // Normalize after the drive root (C:\) at offset PREFIX+3.
  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + 3, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_relative — handle plain relative paths (foo, ./bar).
// Prepends \??\<CWD>\ and normalizes.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult resolve_relative(const char *path,
                                                     WCHAR *buf,
                                                     size_t max_wchars) {
  using namespace path_detail;

  size_t path_len = __builtin_strlen(path);

  // First convert the relative portion into buf after the prefix area.
  // We'll shift it to its final position after measuring CWD.
  auto wr = utf8_to_wide_nt(path, path_len, buf, NT_PREFIX_LEN, max_wchars);
  if (wr.error)
    return wr;
  size_t rel_wide_chars = wr.chars;

  UNICODE_STRING *cwd =
      &NtCurrentPeb()->ProcessParameters->CurrentDirectory.DosPath;
  ULONG cwd_chars = cwd->Length / sizeof(WCHAR);
  if (cwd_chars == 0)
    return {0, ENOENT};

  bool cwd_has_sep = cwd->Buffer[cwd_chars - 1] == u'\\';
  ULONG sep = cwd_has_sep ? 0 : 1;
  size_t total = NT_PREFIX_LEN + cwd_chars + sep + rel_wide_chars;
  if (total >= max_wchars)
    return {0, ENAMETOOLONG};

  // Shift the relative part rightward to make room for the CWD prefix.
  // Backward iteration is required: dest (rel_offset) > src (NT_PREFIX_LEN),
  // so a forward copy would overwrite source data before reading it.
  ULONG rel_offset = NT_PREFIX_LEN + cwd_chars + sep;
  for (size_t i = rel_wide_chars; i > 0; --i)
    buf[rel_offset + i - 1] = buf[NT_PREFIX_LEN + i - 1];

  write_nt_prefix(buf);
  __builtin_memcpy(buf + NT_PREFIX_LEN, cwd->Buffer,
                   cwd_chars * sizeof(WCHAR));
  if (!cwd_has_sep)
    buf[NT_PREFIX_LEN + cwd_chars] = u'\\';
  buf[total] = u'\0';

  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN + 3, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_unc — handle UNC paths (\\server\share\...).
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult resolve_unc(const char *path, WCHAR *buf,
                                                size_t max_wchars) {
  using namespace path_detail;

  size_t path_len = __builtin_strlen(path);
  auto wr = utf8_to_wide_nt(path, path_len, buf, NT_PREFIX_LEN, max_wchars);
  if (wr.error)
    return wr;
  size_t wide_chars = wr.chars;

  // Regular UNC: \\server\share -> \??\UNC\server\share
  static constexpr WCHAR UNC_PREFIX[] = u"\\??\\UNC\\";
  constexpr size_t UNC_PREFIX_LEN = 8;

  if (wide_chars < 2)
    return {0, EINVAL};

  // Content after the leading \\ is at PREFIX+2, move to UNC_PREFIX_LEN.
  ULONG content_start = NT_PREFIX_LEN + 2;
  ULONG content_len = static_cast<ULONG>(wide_chars - 2);
  if (UNC_PREFIX_LEN + content_len >= max_wchars)
    return {0, ENAMETOOLONG};

  // Shift content right (may overlap).
  for (ULONG i = content_len; i > 0; --i)
    buf[UNC_PREFIX_LEN + i - 1] = buf[content_start + i - 1];

  __builtin_memcpy(buf, UNC_PREFIX, UNC_PREFIX_LEN * sizeof(WCHAR));
  size_t total = UNC_PREFIX_LEN + content_len;
  buf[total] = u'\0';

  // Normalize after \??\UNC\server\share\ (skip server and share names).
  size_t norm_start = UNC_PREFIX_LEN;
  while (norm_start < total && buf[norm_start] != u'\\')
    ++norm_start;
  if (norm_start < total)
    ++norm_start;
  while (norm_start < total && buf[norm_start] != u'\\')
    ++norm_start;
  if (norm_start < total)
    ++norm_start;
  size_t norm = normalize_path_components(buf, norm_start, total);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// resolve_verbatim_or_device — handle \\?\ and \\.\ paths.
// \\?\ (verbatim) passes through with NO canonicalization.
// \\.\ (device) IS canonicalized.
// ---------------------------------------------------------------------------
LIBC_INLINE path_detail::WideResult
resolve_verbatim_or_device(const char *path, WCHAR *buf, size_t max_wchars) {
  using namespace path_detail;

  size_t path_len = __builtin_strlen(path);
  auto wr = utf8_to_wide_nt(path, path_len, buf, NT_PREFIX_LEN, max_wchars);
  if (wr.error)
    return wr;
  size_t wide_chars = wr.chars;

  if (wide_chars < 4)
    return {0, EINVAL};

  bool is_verbatim = buf[NT_PREFIX_LEN + 2] == u'?';

  // \\?\C:\... or \\.\Device\... -> \??\C:\... or \??\Device\...
  // Strip the 4-char \\?\ or \\.\ prefix and replace with \??\.
  size_t content_chars = wide_chars - 4;
  for (size_t i = 0; i < content_chars; ++i)
    buf[NT_PREFIX_LEN + i] = buf[NT_PREFIX_LEN + 4 + i];

  write_nt_prefix(buf);
  buf[NT_PREFIX_LEN + content_chars] = u'\0';

  if (is_verbatim) {
    // Verbatim: no normalization, path is passed as-is to NT.
    return {NT_PREFIX_LEN + content_chars, 0};
  }

  // Device path (\\.\): normalize like other Win32 paths.
  size_t norm = normalize_path_components(buf, NT_PREFIX_LEN,
                                          NT_PREFIX_LEN + content_chars);
  return norm ? WideResult{norm, 0} : WideResult{0, ENAMETOOLONG};
}

// ---------------------------------------------------------------------------
// validate_path — basic sanity checks. Returns 0 or errno.
// ---------------------------------------------------------------------------
LIBC_INLINE int validate_path(const char *path) {
  if (!path)
    return EFAULT;
  if (path[0] == '\0')
    return ENOENT;
  // Check total byte length. NT maximum is 32767 UTF-16 code units.
  // UTF-8 byte length >= UTF-16 code unit count (multi-byte UTF-8 sequences
  // encode to equal or fewer UTF-16 code units), so byte length is a safe
  // upper bound for the converted length.
  size_t len = __builtin_strlen(path);
  if (len > MAX_NT_PATH_WCHARS)
    return ENAMETOOLONG;
  return 0;
}

// ---------------------------------------------------------------------------
// resolve_path — the single entry point for all path resolution.
//
// Classifies the path, validates it, and either:
//   - Writes an NT path into buf and returns {kind, 0, nt_len, -1}, or
//   - Returns a virtual classification for caller dispatch, or
//   - Returns {Invalid, errno, 0, -1} on error.
// ---------------------------------------------------------------------------
LIBC_INLINE ResolvedPath resolve_path(const char *path, WCHAR *buf,
                                      size_t max_wchars) {
  using LIBC_NAMESPACE::cpp::string_view;

  int verr = validate_path(path);
  if (verr)
    return {PathKind::Invalid, verr, 0, -1};

  const string_view sv(path);
  PathKind kind = PathKind::Invalid;
  if (sv.starts_with("/dev") && (sv.size() == 4 || sv[4] == '/')) {
    const DevSubpathInfo dev = classify_dev_subpath_info(sv);
    if (dev.error)
      return {PathKind::Invalid, dev.error, 0, -1};
    kind = dev.kind;
  } else {
    kind = classify_path(path);
  }

  // Helper: convert a WideResult from a resolve function into a ResolvedPath.
  auto from_wide = [kind](path_detail::WideResult wr) -> ResolvedPath {
    if (wr.error)
      return {kind, wr.error, 0, -1};
    return {kind, 0, wr.chars, -1};
  };

  switch (kind) {
  case PathKind::Invalid:
    return {PathKind::Invalid, EINVAL, 0, -1};

  case PathKind::DevNull:
  case PathKind::DevZero:
  case PathKind::DevRandom:
  case PathKind::DevUrandom:
  case PathKind::DevTty:
    return from_wide(resolve_dev_device(kind, buf, max_wchars));

  case PathKind::DevPty:
    return {PathKind::DevPty, 0, 0, -1};

  case PathKind::DevFd: {
    int fd = resolve_dev_fd(path);
    if (fd < 0)
      return {PathKind::DevFd, ENOENT, 0, -1};
    return {PathKind::DevFd, 0, 0, fd};
  }

  case PathKind::PosixTmp:
    return from_wide(resolve_tmp_path(path, buf, max_wchars));
  case PathKind::PosixAbsolute:
    return from_wide(resolve_posix_absolute(path, buf, max_wchars));
  case PathKind::DosDriveRelative:
    return from_wide(resolve_drive_relative(path, buf, max_wchars));
  case PathKind::DosRooted:
    return from_wide(resolve_rooted_path(path, buf, max_wchars));
  case PathKind::DosAbsolute:
    return from_wide(resolve_dos_absolute(path, buf, max_wchars));
  case PathKind::DosRelative:
    return from_wide(resolve_relative(path, buf, max_wchars));
  case PathKind::Unc:
    return from_wide(resolve_unc(path, buf, max_wchars));
  case PathKind::VerbatimOrDevice:
    return from_wide(resolve_verbatim_or_device(path, buf, max_wchars));
  }

  return {PathKind::Invalid, EINVAL, 0, -1};
}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PATH_RESOLVER_H
