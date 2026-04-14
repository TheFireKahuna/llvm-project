//===-- Shared process creation utilities -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities shared between posix_spawn and exec*. Provides command-line
// quoting, environment block construction, RuntimeData building, and a
// shared NtCreateUserProcess launcher.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_UTILS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_UTILS_H

#include "hdr/errno_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/OSUtil/windows/nt/unicode_string_utils.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"

namespace LIBC_NAMESPACE_DECL {
namespace process_utils {

inline constexpr DWORD PROCESS_PARAMS_USE_STD_HANDLES = 0x00000100;

// --- Temporary buffer backed by mmap ---
// Single allocation for all scratch data (wide strings, handle arrays,
// RuntimeData). Freed in the destructor.
struct TempBuffer {
  void *base = nullptr;
  size_t size = 0;

  bool alloc(size_t bytes) {
    size = bytes;
    base = LIBC_NAMESPACE::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
      base = nullptr;
      return false;
    }
    return true;
  }

  ~TempBuffer() {
    if (base)
      LIBC_NAMESPACE::munmap(base, size);
  }
};

inline bool mark_handle_inheritable(HANDLE handle) {
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return false;

  OBJECT_HANDLE_FLAG_INFORMATION flags = {TRUE, FALSE};
  return NT_SUCCESS(::NtSetInformationObject(
      handle, ObjectHandleFlagInformation, &flags, sizeof(flags)));
}

inline void append_unique_handle(HANDLE *handles, DWORD *count, HANDLE handle) {
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return;
  for (DWORD i = 0; i < *count; ++i) {
    if (handles[i] == handle)
      return;
  }
  handles[(*count)++] = handle;
}

// --- UTF-8 to UTF-16 conversion helpers ---

inline int utf8_to_wide_len(const char *s) {
  return windows::utf8_to_wide_len(s);
}

inline int utf8_to_wide(const char *s, WCHAR *buf, int buf_len) {
  return windows::utf8_to_wide(s, buf, buf_len);
}

// --- Command line quoting (MSVC/UCRT compatible) ---
//
// Implements the inverse of the Windows CRT startup parser: each argument is
// quoted so the child reconstructs the original argv. Rules:
//   - Always surround with double quotes (safe for all content).
//   - Backslashes before a double quote are doubled.
//   - Backslashes at end of argument (before closing quote) are doubled.
//   - Internal double quotes are escaped as \".

// Compute the quoted length of a single argument (including quotes and space).
inline size_t quoted_arg_len(const WCHAR *arg) {
  size_t len = 3; // opening quote + closing quote + trailing space
  size_t backslashes = 0;
  for (const WCHAR *p = arg; *p; ++p) {
    if (*p == u'\\') {
      ++backslashes;
    } else if (*p == u'"') {
      len += backslashes + 1; // double existing backslashes + escape char
      backslashes = 0;
    } else {
      backslashes = 0;
    }
    ++len;
  }
  len += backslashes; // double trailing backslashes before closing quote
  return len;
}

// Write a quoted argument to dst. Returns pointer past the written data.
inline WCHAR *write_quoted_arg(WCHAR *dst, const WCHAR *arg) {
  *dst++ = u'"';
  size_t backslashes = 0;
  for (const WCHAR *p = arg; *p; ++p) {
    if (*p == u'\\') {
      ++backslashes;
    } else if (*p == u'"') {
      // Double all backslashes before this quote, then escape the quote.
      for (size_t i = 0; i < backslashes; ++i)
        *dst++ = u'\\';
      backslashes = 0;
      *dst++ = u'\\';
      *dst++ = u'"';
      continue;
    } else {
      backslashes = 0;
    }
    *dst++ = *p;
  }
  // Double trailing backslashes (they precede the closing quote).
  for (size_t i = 0; i < backslashes; ++i)
    *dst++ = u'\\';
  *dst++ = u'"';
  return dst;
}

// --- Build wide command line from argv ---
// Returns the required buffer size in bytes (including null terminator),
// or 0 on error. If dst is non-null, writes the command line to it.
inline SIZE_T build_cmdline(char *const *argv, WCHAR *dst) {
  if (!argv || !argv[0])
    return 0;

  // First pass: compute total wide chars needed.
  SIZE_T total_wchars = 1; // null terminator
  for (int i = 0; argv[i]; ++i) {
    int wide_len = utf8_to_wide_len(argv[i]);
    if (wide_len <= 0)
      return 0;
    // Worst case: every char needs escaping -> 2x + 3 (quotes + space)
    total_wchars += static_cast<SIZE_T>(wide_len) * 2 + 3;
  }

  if (!dst)
    return total_wchars * sizeof(WCHAR);

  // Second pass: convert each arg and quote it.
  WCHAR *out = dst;
  // Use a portion of the destination buffer as a scratch area for
  // individual argument conversion. Place it at the end of the buffer.
  WCHAR *scratch = dst + total_wchars / 2;
  for (int i = 0; argv[i]; ++i) {
    if (i > 0)
      *out++ = u' ';
    int wide_len =
        utf8_to_wide(argv[i], scratch, static_cast<int>(total_wchars / 2));
    if (wide_len <= 0)
      return 0;
    // wide_len includes null terminator — don't quote that.
    scratch[wide_len - 1] = u'\0';
    out = write_quoted_arg(out, scratch);
  }
  *out = u'\0';
  return static_cast<SIZE_T>(out - dst + 1) * sizeof(WCHAR);
}

// --- Build wide environment block from envp ---
// Windows format: "VAR=val\0VAR2=val2\0\0" in UTF-16.
// Returns required buffer size in bytes, or 0 to inherit parent environment.
inline SIZE_T build_env_block(char *const *envp, WCHAR *dst) {
  if (!envp)
    return 0; // Inherit parent environment.

  SIZE_T total_wchars = 1; // final double-null
  for (int i = 0; envp[i]; ++i) {
    int len = utf8_to_wide_len(envp[i]);
    if (len <= 0)
      return 0;
    total_wchars += static_cast<SIZE_T>(len);
  }

  if (!dst)
    return total_wchars * sizeof(WCHAR);

  WCHAR *out = dst;
  for (int i = 0; envp[i]; ++i) {
    int len = utf8_to_wide(envp[i], out, static_cast<int>(total_wchars));
    if (len <= 0)
      return 0;
    out += len; // past the null terminator for this string
    total_wchars -= static_cast<SIZE_T>(len);
  }
  *out = u'\0'; // double-null terminator
  return static_cast<SIZE_T>(out - dst + 1) * sizeof(WCHAR);
}

// --- MSVC CRT lpReserved2 protocol for fd inheritance ---
//
// Layout: [DWORD count][BYTE flags[count]][HANDLE handles[count]]
// This is the de facto standard for passing inherited fds on Windows,
// used by MSVC CRT, UCRT, and MinGW. Our init_std_fds parses it so
// the child's fd_table gets populated for all inherited fds.

// MSVC CRT per-fd flags.
inline constexpr BYTE FOPEN = 0x01;
inline constexpr BYTE FAPPEND = 0x20;
inline constexpr BYTE FTEXT = 0x80;

inline SIZE_T reserved2_size(int max_fd) {
  return sizeof(DWORD) + static_cast<SIZE_T>(max_fd) * (1 + sizeof(HANDLE));
}

// Build the lpReserved2 block. fd_handles is an array of max_fd HANDLEs
// (INVALID_HANDLE_VALUE or nullptr for unused fds).
inline void build_reserved2(BYTE *dst, HANDLE *fd_handles, int max_fd) {
  // Write count.
  auto *count_ptr = reinterpret_cast<DWORD *>(dst);
  *count_ptr = static_cast<DWORD>(max_fd);

  // Write per-fd flags.
  BYTE *flags = dst + sizeof(DWORD);
  for (int i = 0; i < max_fd; ++i) {
    if (fd_handles[i] && fd_handles[i] != INVALID_HANDLE_VALUE)
      flags[i] = FOPEN;
    else
      flags[i] = 0;
  }

  // Write handles.
  auto *handles = reinterpret_cast<HANDLE *>(flags + max_fd);
  for (int i = 0; i < max_fd; ++i)
    handles[i] = fd_handles[i] ? fd_handles[i] : INVALID_HANDLE_VALUE;
}

// --- Cursor alignment helpers ---

inline char *align_to(char *cursor, uintptr_t alignment) {
  return reinterpret_cast<char *>(
      (reinterpret_cast<uintptr_t>(cursor) + alignment - 1) & ~(alignment - 1));
}

struct NativeProcessLaunchOptions {
  const WCHAR *image_path = nullptr;
  SIZE_T image_path_chars = 0;
  const WCHAR *nt_image_path = nullptr;
  SIZE_T nt_image_path_chars = 0;
  const WCHAR *command_line = nullptr;
  SIZE_T command_line_bytes = 0;
  void *environment = nullptr;
  HANDLE *inherit_handles = nullptr;
  DWORD inherit_handle_count = 0;
  HANDLE std_in = nullptr;
  HANDLE std_out = nullptr;
  HANDLE std_err = nullptr;
  HANDLE console_handle = nullptr;
  BYTE *runtime_data = nullptr;
  SIZE_T runtime_data_size = 0;
  HANDLE token = nullptr;
  bool has_priority_class = false;
  UCHAR priority_class = PROCESS_PRIORITY_CLASS_NORMAL;
  bool set_process_group = false;
  DWORD process_group_id = 0;
  bool resume_immediately = true;
  // If true, skip mark_handle_inheritable for inherit_handles — the caller
  // has already set OBJ_INHERIT on duplicated handles and a concurrent
  // fcntl(F_SETFD) may have revoked it. Re-arming would undo the revocation.
  bool handles_pre_inherited = false;
};

struct NativeProcessLaunchResult {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  DWORD process_id = 0;
  DWORD thread_id = 0;
};

// Delegate to the shared windows_util version.
inline bool init_unicode_string_ex(UNICODE_STRING *dest, const WCHAR *src,
                                   SIZE_T char_count) {
  return windows_util::init_unicode_string_safe(dest, src, char_count);
}

inline int launch_user_process_native(const NativeProcessLaunchOptions &opts,
                                      NativeProcessLaunchResult *out) {
  if (!opts.image_path || !opts.nt_image_path || !out) {
    return EINVAL;
  }
  if (opts.image_path_chars == 0 || opts.nt_image_path_chars == 0) {
    return EINVAL;
  }
  if (opts.runtime_data_size > 0xFFFE || opts.command_line_bytes > 0x10000) {
    return E2BIG;
  }

  auto *parent_params = NtCurrentPeb()->ProcessParameters;
  if (!parent_params) {
    return EIO;
  }

  UNICODE_STRING image_path = {};
  UNICODE_STRING nt_image_path = {};
  UNICODE_STRING command_line = {};
  UNICODE_STRING runtime_data = {};
  if (!init_unicode_string_ex(&image_path, opts.image_path,
                              opts.image_path_chars) ||
      !init_unicode_string_ex(&nt_image_path, opts.nt_image_path,
                              opts.nt_image_path_chars)) {
    return ENAMETOOLONG;
  }
  if (opts.command_line && opts.command_line_bytes) {
    SIZE_T cmd_chars = (opts.command_line_bytes / sizeof(WCHAR)) - 1;
    if (!init_unicode_string_ex(&command_line, opts.command_line, cmd_chars)) {
      return E2BIG;
    }
  }
  if (opts.runtime_data && opts.runtime_data_size) {
    runtime_data.Length = static_cast<USHORT>(opts.runtime_data_size);
    runtime_data.MaximumLength = static_cast<USHORT>(opts.runtime_data_size);
    runtime_data.Buffer =
        reinterpret_cast<WCHAR *>(const_cast<BYTE *>(opts.runtime_data));
  }

  RTL_USER_PROCESS_PARAMETERS *proc_params = nullptr;
  // TODO(ntposix): Pre-create packaged-app / execution-alias / SxS rewriting
  // still lives in KernelBase!CreateProcessInternalW. Verified retry forms
  // from the local reverse-engineering notes include:
  //   - PsCreateFailOnFileOpen: BasepGetPackagedAppInfoForFile(..., &blob)
  //     then blob-type-driven rewrites of the effective image / target slot,
  //     current-directory-like state, and replacement command line.
  //   - PsCreateFailExeName: IFEO "Debugger" retry as
  //       debugger + L" " + old_command_line
  //     plus an alternate
  //       LoadAppExecutionAliasInfoForExecutable(ifeo_key_or_null,
  //                                             current_target,
  //                                             old_aux_path_like,
  //                                             heap,
  //                                             &blob)
  //     path driven by AppExecutionAliasRedirectPackages /
  //     AppExecutionAliasRedirect.
  // Native launch here assumes the caller already resolved a concrete image
  // path and does not need those KernelBase retry surfaces.
  NTSTATUS st = ::RtlCreateProcessParametersEx(
      &proc_params, &image_path, &parent_params->DllPath,
      &parent_params->CurrentDirectory.DosPath,
      opts.command_line ? &command_line : nullptr, opts.environment,
      &parent_params->WindowTitle, &parent_params->DesktopInfo,
      &parent_params->ShellInfo, opts.runtime_data ? &runtime_data : nullptr,
      RTL_USER_PROC_PARAMS_NORMALIZED);
  if (!NT_SUCCESS(st))
    return windows_util::ntstatus_to_errno(st);

  // Native NtCreateUserProcess preserves ShellInfo reliably, while the
  // RuntimeData field is not surfaced back to the child in our direct
  // launch path. Alias the bootstrap blob through ShellInfo so early
  // libc startup can recover the reserved2-style payload in native children.
  if (opts.runtime_data && opts.runtime_data_size)
    proc_params->ShellInfo = proc_params->RuntimeData;

  // Console inheritance for NtCreateUserProcess (bypassing Win32):
  //
  // We cannot copy the parent's ConsoleHandle directly — it is a console-server
  // reference that is process-local; the child's kernel32!ConsoleInitialize
  // rejects it with STATUS_DLL_INIT_FAILED (0xC0000142).
  //
  // Sentinel values that csrss recognises in ConsoleHandle:
  //   (HANDLE)-1  DETACHED_PROCESS    — no console allocated
  //   (HANDLE)-2  CREATE_NEW_CONSOLE  — new console + visible window
  //   (HANDLE)-3  CREATE_NO_WINDOW    — console allocated, no visible window
  //   0 / NULL                        — csrss creates a new visible console
  //
  // Strategy:
  //   - if opts.console_handle is provided, the caller has supplied the exact
  //     ConsoleHandle field to publish in the child, either a live inherited
  //     console session handle or one of the csrss sentinel values above.
  //   - otherwise, if the parent is already detached, propagate detached.
  //   - otherwise give the child a hidden console so kernel32 console APIs
  //     work in the child (if it loads kernel32) but no window appears.
  // Actual byte I/O still goes through the explicit
  // StandardInput/StandardOutput/StandardError handles set below.
  const auto CONSOLE_DETACHED = reinterpret_cast<HANDLE>(
      internal::console_util::CONSOLE_HANDLE_DETACHED);
  const auto CONSOLE_NO_WINDOW = reinterpret_cast<HANDLE>(
      internal::console_util::CONSOLE_HANDLE_NO_WINDOW);
  HANDLE parent_console = parent_params->ConsoleHandle;
  if (opts.console_handle) {
    proc_params->ConsoleHandle = opts.console_handle;
  } else if (parent_console == CONSOLE_DETACHED) {
    proc_params->ConsoleHandle = CONSOLE_DETACHED;
  } else {
    proc_params->ConsoleHandle = CONSOLE_NO_WINDOW;
  }
  proc_params->ConsoleFlags = parent_params->ConsoleFlags;
  proc_params->StandardInput = opts.std_in;
  proc_params->StandardOutput = opts.std_out;
  proc_params->StandardError = opts.std_err;
  proc_params->CurrentDirectory.Handle = parent_params->CurrentDirectory.Handle;
  proc_params->StartingX = parent_params->StartingX;
  proc_params->StartingY = parent_params->StartingY;
  proc_params->CountX = parent_params->CountX;
  proc_params->CountY = parent_params->CountY;
  proc_params->CountCharsX = parent_params->CountCharsX;
  proc_params->CountCharsY = parent_params->CountCharsY;
  proc_params->FillAttribute = parent_params->FillAttribute;
  proc_params->WindowFlags =
      parent_params->WindowFlags | PROCESS_PARAMS_USE_STD_HANDLES;
  proc_params->ShowWindowFlags = parent_params->ShowWindowFlags;
  // Match BasepCreateProcessParameters: explicit groups are seeded directly,
  // while a new leader stays 0 until child-side ConsoleInitialize rewrites it
  // to the child's PID before console registration.
  proc_params->ProcessGroupId = opts.set_process_group
                                    ? opts.process_group_id
                                    : parent_params->ProcessGroupId;

  CLIENT_ID client_id = {};
  struct {
    SIZE_T TotalLength;
    PS_ATTRIBUTE Attributes[5];
  } attr_list = {};
  DWORD attr_count = 0;

  attr_list.Attributes[attr_count].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
  attr_list.Attributes[attr_count].Size = nt_image_path.Length;
  attr_list.Attributes[attr_count].ValuePtr = nt_image_path.Buffer;
  attr_list.Attributes[attr_count].ReturnLength = nullptr;
  ++attr_count;

  attr_list.Attributes[attr_count].Attribute = PS_ATTRIBUTE_CLIENT_ID;
  attr_list.Attributes[attr_count].Size = sizeof(client_id);
  attr_list.Attributes[attr_count].ValuePtr = &client_id;
  attr_list.Attributes[attr_count].ReturnLength = nullptr;
  ++attr_count;

  if (opts.inherit_handle_count) {
    if (!opts.handles_pre_inherited) {
      for (DWORD i = 0; i < opts.inherit_handle_count; ++i)
        (void)mark_handle_inheritable(opts.inherit_handles[i]);
    }
    attr_list.Attributes[attr_count].Attribute = PS_ATTRIBUTE_HANDLE_LIST;
    attr_list.Attributes[attr_count].Size =
        static_cast<SIZE_T>(opts.inherit_handle_count) * sizeof(HANDLE);
    attr_list.Attributes[attr_count].ValuePtr = opts.inherit_handles;
    attr_list.Attributes[attr_count].ReturnLength = nullptr;
    ++attr_count;
  }

  if (opts.token) {
    attr_list.Attributes[attr_count].Attribute = PS_ATTRIBUTE_TOKEN;
    attr_list.Attributes[attr_count].Size = sizeof(HANDLE);
    attr_list.Attributes[attr_count].ValuePtr = opts.token;
    attr_list.Attributes[attr_count].ReturnLength = nullptr;
    ++attr_count;
  }

  if (opts.has_priority_class) {
    attr_list.Attributes[attr_count].Attribute = PS_ATTRIBUTE_PRIORITY_CLASS;
    attr_list.Attributes[attr_count].Size = sizeof(UCHAR);
    attr_list.Attributes[attr_count].ValuePtr =
        const_cast<UCHAR *>(&opts.priority_class);
    attr_list.Attributes[attr_count].ReturnLength = nullptr;
    ++attr_count;
  }

  attr_list.TotalLength =
      sizeof(SIZE_T) + static_cast<SIZE_T>(attr_count) * sizeof(PS_ATTRIBUTE);

  PS_CREATE_INFO create_info = {};
  create_info.Size = sizeof(create_info);
  create_info.State = PsCreateInitialState;

  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  ULONG process_flags = opts.inherit_handle_count
                            ? PROCESS_CREATE_FLAGS_INHERIT_HANDLES
                            : PROCESS_CREATE_FLAGS_NONE;
  st = ::NtCreateUserProcess(
      &process, &thread, MAXIMUM_ALLOWED, MAXIMUM_ALLOWED, nullptr, nullptr,
      process_flags, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, proc_params,
      &create_info, reinterpret_cast<PS_ATTRIBUTE_LIST *>(&attr_list));
  (void)::RtlDestroyProcessParameters(proc_params);

  // TODO(ntposix): Match CreateProcessInternalW's verified PS_CREATE_INFO
  // recovery paths:
  //   - PsCreateFailOnFileOpen: packaged-app / execution-alias rewrite + retry
  //   - PsCreateFailOnSectionCreate:
  //       %ComSpec% /c <old command line>
  //       or %SystemRoot%\\system32\\cmd.exe /c <old command line>
  //     plus invalid-image / VDM-style handling
  //   - PsCreateFailExeName:
  //       Debugger + L" " + old_command_line
  //     plus the alternate execution-alias rewrite path driven by IFEO
  //     AppExecutionAliasRedirectPackages / AppExecutionAliasRedirect or the
  //     direct alias-lookup fallback
  //   - PsCreateFailMachineMismatch: STATUS_IMAGE_MACHINE_TYPE_MISMATCH_EXE
  //     hard-error path before final Win32 error translation
  if (!NT_SUCCESS(st)) {
    if (create_info.State == PsCreateFailExeFormat ||
        create_info.State == PsCreateFailMachineMismatch)
      return ENOEXEC;
    return windows_util::ntstatus_to_errno(st);
  }

  DWORD child_pid =
      static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(client_id.UniqueProcess));
  DWORD child_tid =
      static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(client_id.UniqueThread));

  out->process = process;
  out->thread = thread;
  out->process_id = child_pid;
  out->thread_id = child_tid;
  if (!opts.resume_immediately)
    return 0;

  st = ::NtAlertResumeThread(thread, nullptr);
  if (!NT_SUCCESS(st)) {
    ::NtTerminateProcess(process, 127);
    ::NtClose(thread);
    ::NtClose(process);
    out->process = nullptr;
    out->thread = nullptr;
    out->process_id = 0;
    out->thread_id = 0;
    return windows_util::ntstatus_to_errno(st);
  }
  return 0;
}

} // namespace process_utils
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_UTILS_H
