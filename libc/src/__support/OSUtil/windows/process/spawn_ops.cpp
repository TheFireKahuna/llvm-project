//===-- Windows internal posix_spawn engine --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel function for posix_spawn on Windows. Implements Linux syscall
// semantics: 0 on success, -errno on failure. Called from
// windows_syscalls::posix_spawn wrapper.
//
// Maps posix_spawn to NtCreateUserProcess. Key translations:
//   - argv array -> quoted Windows command line string
//   - envp array -> null-terminated UTF-16 environment block
//   - file_actions (fds 0/1/2) -> RTL_USER_PROCESS_PARAMETERS std handles
//   - PS_ATTRIBUTE_HANDLE_LIST for secure handle inheritance
//   - Child registered in the process-wide child table for waitpid/SIGCHLD
//
//===----------------------------------------------------------------------===//

#include "spawn_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "hdr/spawn_macros.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/CPP/utility.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/fd_ops.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/process/pty_reserved2.h"
#include "src/__support/OSUtil/windows/process/pty_tree.h"
#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "src/__support/OSUtil/windows/process/process_utils.h"
#include "src/__support/OSUtil/windows/process/shebang.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/process/windows/child_table.h"
#include "src/spawn/file_actions.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

using process_utils::align_to;
using process_utils::build_cmdline;
using process_utils::build_env_block;
using process_utils::build_reserved2;
using process_utils::reserved2_size;
using process_utils::utf8_to_wide;
using process_utils::utf8_to_wide_len;

namespace {

// --- File actions: apply OPEN/CLOSE/DUP2 for all fds ---
//
// On Windows, there's no fork. File actions are applied in the parent:
//   - Fds 0/1/2 -> RTL_USER_PROCESS_PARAMETERS standard handles
//   - PTY slave dup2 on 0/1/2 -> explicit console-session inheritance
//   - All fds -> RuntimeData block (MSVC CRT protocol)
//   - PS_ATTRIBUTE_HANDLE_LIST for secure inheritance
//
// fd_handles[] is populated with inheritable HANDLEs indexed by fd number.
// closed_fds[] tracks fds that should NOT be inherited. Returns 0 or errno.

// Maximum handles we can track for cleanup on error. Stack-allocated.
inline constexpr int MAX_CLEANUP_HANDLES = 64;

struct FileActionState {
  HANDLE *fd_handles;      // dynamically allocated, indexed by fd
  uintptr_t *pty_session_keys;
  int fd_handles_capacity; // allocated size
  int max_fd;              // highest fd + 1
  uint16_t pty_stdio_mask;
  HANDLE to_close[MAX_CLEANUP_HANDLES];
  int close_count;
  vt_pty::SpawnHandles pty_spawn;
  bool has_pty_spawn;

  void init(HANDLE *handle_buf, uintptr_t *pty_key_buf, int handle_capacity) {
    fd_handles = handle_buf;
    pty_session_keys = pty_key_buf;
    fd_handles_capacity = handle_capacity;
    for (int i = 0; i < handle_capacity; ++i)
      fd_handles[i] = nullptr;
    for (int i = 0; i < handle_capacity; ++i)
      pty_session_keys[i] = 0;
    max_fd = 3; // minimum: stdin/stdout/stderr
    pty_stdio_mask = 0;
    close_count = 0;
    pty_spawn = {};
    has_pty_spawn = false;

    // Seed fds 0/1/2 from the parent's fd_table (not PEB). The fd_table
    // reflects any dup2/close/open the parent has done since startup —
    // PEB->ProcessParameters->Standard* are stale after redirection.
    for (int i = 0; i < 3 && i < handle_capacity; ++i) {
      auto result = internal::fd_table.get(i);
      if (result.has_value())
        fd_handles[i] = result.value();
    }
  }

  void cleanup() {
    if (has_pty_spawn)
      vt_pty::close_spawn_handles(&pty_spawn);
    for (int i = 0; i < close_count; ++i)
      ::NtClose(to_close[i]);
  }
};

static bool mark_handle_inheritable(HANDLE handle) {
  return process_utils::mark_handle_inheritable(handle);
}

static uint16_t pty_stdio_bit_for_fd(int fd) {
  switch (fd) {
  case 0:
    return LLVM_LIBC_PTY_ATTACH_STDIN;
  case 1:
    return LLVM_LIBC_PTY_ATTACH_STDOUT;
  case 2:
    return LLVM_LIBC_PTY_ATTACH_STDERR;
  default:
    return 0;
  }
}

static int apply_file_actions(const posix_spawn_file_actions_t *actions,
                              FileActionState &state) {
  if (!actions)
    return 0;

  auto *act = reinterpret_cast<BaseSpawnFileAction *>(actions->__front);
  while (act) {
    switch (act->type) {
    case BaseSpawnFileAction::OPEN: {
      auto *open_act = reinterpret_cast<SpawnFileOpenAction *>(act);
      int fd = open_act->fd;
      if (fd < 0 || fd >= state.fd_handles_capacity) {
        return EINVAL;
      }

      int opened_fd = static_cast<int>(
          internal::open(open_act->path, open_act->oflag, open_act->mode));
      if (opened_fd < 0)
        return -opened_fd;

      auto opened_handle = internal::fd_table.get(opened_fd);
      if (!opened_handle.has_value()) {
        (void)internal::close(opened_fd);
        return opened_handle.error();
      }

      HANDLE inherited = nullptr;
      NTSTATUS st = ::NtDuplicateObject(
          NtCurrentProcess(), opened_handle.value(), NtCurrentProcess(),
          &inherited, 0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
      int close_result = static_cast<int>(internal::close(opened_fd));
      if (!NT_SUCCESS(st))
        return windows_util::ntstatus_to_errno(st);
      if (close_result < 0)
        return -close_result;

      state.fd_handles[fd] = inherited;
      state.pty_session_keys[fd] = 0;
      state.pty_stdio_mask = static_cast<uint16_t>(
          state.pty_stdio_mask & ~pty_stdio_bit_for_fd(fd));
      if (fd + 1 > state.max_fd)
        state.max_fd = fd + 1;
      state.to_close[state.close_count++] = inherited;
      break;
    }
    case BaseSpawnFileAction::CLOSE: {
      auto *close_act = reinterpret_cast<SpawnFileCloseAction *>(act);
      int fd = close_act->fd;
      if (fd >= 0 && fd < state.fd_handles_capacity) {
        state.fd_handles[fd] = INVALID_HANDLE_VALUE;
        state.pty_session_keys[fd] = 0;
        state.pty_stdio_mask = static_cast<uint16_t>(
            state.pty_stdio_mask & ~pty_stdio_bit_for_fd(fd));
      }
      break;
    }
    case BaseSpawnFileAction::DUP2: {
      auto *dup_act = reinterpret_cast<SpawnFileDup2Action *>(act);
      int src_fd = dup_act->fd;
      int dst_fd = dup_act->newfd;
      if (dst_fd < 0 || dst_fd >= state.fd_handles_capacity) {
        return EINVAL;
      }

      uintptr_t pty_key = 0;
      if (src_fd >= 0 && src_fd < state.fd_handles_capacity)
        pty_key = state.pty_session_keys[src_fd];

      OpenFileDescription *src_ofd = nullptr;
      if (pty_key == 0)
        src_ofd = internal::fd_table.get_ofd(src_fd);

      if (pty_key != 0 || (src_ofd && vt_pty::is_slave(src_ofd))) {
        if (dst_fd > 2) {
          return EINVAL;
        }

        if (pty_key == 0)
          pty_key = vt_pty::session_key(src_ofd);

        if (!state.has_pty_spawn) {
          auto pty_result =
              vt_pty::duplicate_spawn_handles(src_ofd, &state.pty_spawn);
          if (!pty_result.has_value()) {
            return pty_result.error();
          }
          state.has_pty_spawn = true;
        } else if (state.pty_spawn.session_key != pty_key) {
          return EINVAL;
        }

        state.fd_handles[dst_fd] = nullptr;
        state.pty_session_keys[dst_fd] = pty_key;
        state.pty_stdio_mask = static_cast<uint16_t>(
            state.pty_stdio_mask | pty_stdio_bit_for_fd(dst_fd));
        if (dst_fd + 1 > state.max_fd)
          state.max_fd = dst_fd + 1;
        break;
      }

      // Resolve source handle: first check file action state (handles
      // we've already opened/duped), then fall back to the fd_table.
      HANDLE src = nullptr;
      if (src_fd >= 0 && src_fd < state.fd_handles_capacity &&
          state.fd_handles[src_fd] &&
          state.fd_handles[src_fd] != INVALID_HANDLE_VALUE) {
        src = state.fd_handles[src_fd];
      } else {
        // Look up in the parent's fd_table.
        auto result = internal::fd_table.get(src_fd);
        if (!result.has_value())
          return EBADF;
        src = result.value();
      }

      HANDLE dup = nullptr;
      NTSTATUS st =
          ::NtDuplicateObject(NtCurrentProcess(), src, NtCurrentProcess(), &dup,
                              0, OBJ_INHERIT, DUPLICATE_SAME_ACCESS);
      if (NT_ERROR(st))
        return windows_util::ntstatus_to_errno(st);

      state.fd_handles[dst_fd] = dup;
      state.pty_session_keys[dst_fd] = 0;
      state.pty_stdio_mask = static_cast<uint16_t>(
          state.pty_stdio_mask & ~pty_stdio_bit_for_fd(dst_fd));
      if (dst_fd + 1 > state.max_fd)
        state.max_fd = dst_fd + 1;
      state.to_close[state.close_count++] = dup;
      break;
    }
    }
    act = act->next;
  }
  return 0;
}

// Returns 0 on success. Sets *token_out to a de-elevated primary token if
// the process is UAC-elevated (caller must close). Returns 0 with
// *token_out == nullptr if de-elevation is not needed (not elevated).
static int get_delevated_token(HANDLE *token_out) {
  *token_out = nullptr;

  HANDLE proc_token = nullptr;
  NTSTATUS st = ::NtOpenProcessTokenEx(
      NtCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, 0, &proc_token);
  if (!NT_SUCCESS(st))
    return EPERM;

  DWORD elev_type = 0;
  ULONG ret_len = 0;
  st = ::NtQueryInformationToken(proc_token, TokenElevationType, &elev_type,
                                 sizeof(elev_type), &ret_len);
  if (!NT_SUCCESS(st) || elev_type != TokenElevationTypeFull) {
    ::NtClose(proc_token);
    return 0; // Not elevated -- RESETIDS is a no-op.
  }

  // Get linked (standard-user) token.
  HANDLE linked = nullptr;
  st = ::NtQueryInformationToken(proc_token, TokenLinkedToken, &linked,
                                 sizeof(linked), &ret_len);
  ::NtClose(proc_token);
  if (!NT_SUCCESS(st))
    return EPERM;

  // Linked token is impersonation-level -- duplicate as primary.
  auto oa = windows::internal_oa();
  HANDLE primary = nullptr;
  st = ::NtDuplicateToken(linked, MAXIMUM_ALLOWED, &oa, FALSE, TokenTypePrimary,
                          &primary);
  ::NtClose(linked);
  if (!NT_SUCCESS(st))
    return EPERM;

  *token_out = primary;
  return 0;
}

} // anonymous namespace

intptr_t posix_spawn(pid_t *__restrict pid_out, const char *__restrict path,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *__restrict attr,
                 char *const *__restrict argv, char *const *__restrict envp,
                 const char *raw_cmdline) {
  if (!path) {
    return -EINVAL;
  }

  // --- Read spawn attributes ---
  short attr_flags = attr ? attr->__flags : 0;
  uint64_t sigmask_bits = 0;
  uint64_t sigdefault_bits = 0;
  if (attr) {
    if (attr_flags & POSIX_SPAWN_SETSIGMASK)
      sigmask_bits = signal_state::sigset_to_bits(attr->__sigmask);
    if (attr_flags & POSIX_SPAWN_SETSIGDEF)
      sigdefault_bits = signal_state::sigset_to_bits(attr->__sigdefault);
  }

  // --- Phase 1: Compute buffer sizes ---
  int path_wide_len = utf8_to_wide_len(path);
  if (path_wide_len <= 0) {
    return -EINVAL;
  }

  // When raw_cmdline is provided (e.g. by popen for cmd.exe /c), use it
  // as the pre-formatted UTF-8 command line instead of building from argv.
  // This avoids per-argument quoting that breaks programs like cmd.exe
  // which parse the raw command line text for switches like /c.
  int raw_cmdline_wide_len = 0;
  SIZE_T cmdline_bytes = 0;
  if (raw_cmdline) {
    raw_cmdline_wide_len = utf8_to_wide_len(raw_cmdline);
    if (raw_cmdline_wide_len > 0)
      cmdline_bytes = static_cast<SIZE_T>(raw_cmdline_wide_len) * sizeof(WCHAR);
  } else {
    cmdline_bytes = build_cmdline(argv, nullptr);
  }
  SIZE_T env_bytes = build_env_block(envp, nullptr);

  // fd_handles capacity: enough for any fd referenced by file actions,
  // minimum 3. Scan file actions to find the highest fd.
  int fa_max_fd = 3;
  if (file_actions) {
    auto *scan = reinterpret_cast<BaseSpawnFileAction *>(file_actions->__front);
    while (scan) {
      int fd = -1;
      switch (scan->type) {
      case BaseSpawnFileAction::OPEN:
        fd = reinterpret_cast<SpawnFileOpenAction *>(scan)->fd;
        break;
      case BaseSpawnFileAction::DUP2:
        fd = reinterpret_cast<SpawnFileDup2Action *>(scan)->newfd;
        break;
      default:
        break;
      }
      if (fd + 1 > fa_max_fd)
        fa_max_fd = fd + 1;
      scan = scan->next;
    }
  }

  SIZE_T fd_handles_bytes = static_cast<SIZE_T>(fa_max_fd) * sizeof(HANDLE);
  SIZE_T pty_keys_bytes = static_cast<SIZE_T>(fa_max_fd) * sizeof(uintptr_t);
  SIZE_T std_r2_size = reserved2_size(fa_max_fd);
  SIZE_T reserved2_bytes =
      ((std_r2_size + 7) & ~SIZE_T{7}) + signal_state::RESERVED2_EXT_SIZE +
      PTY_RESERVED2_EXT_SIZE;

  // Extra space for state, current-directory, and console handles.
  SIZE_T total = static_cast<SIZE_T>(path_wide_len) * sizeof(WCHAR) +
                 cmdline_bytes + env_bytes + fd_handles_bytes +
                 pty_keys_bytes +
                 reserved2_bytes +
                 (static_cast<SIZE_T>(fa_max_fd) + 4) * sizeof(HANDLE) +
                 512 /*padding + alignment*/;

  // --- Phase 2: Single scratch allocation for all temporary data ---
  auto buf = windows::byte_scratch(total);
  if (!buf)
    return -ENOMEM;

  char *cursor = buf.data();

  // Wide path.
  WCHAR *wpath = reinterpret_cast<WCHAR *>(cursor);
  utf8_to_wide(path, wpath, path_wide_len);
  cursor += static_cast<size_t>(path_wide_len) * sizeof(WCHAR);

  cursor = align_to(cursor, sizeof(WCHAR));

  // Wide command line.
  WCHAR *wcmdline = nullptr;
  if (cmdline_bytes) {
    wcmdline = reinterpret_cast<WCHAR *>(cursor);
    if (raw_cmdline) {
      utf8_to_wide(raw_cmdline, wcmdline, raw_cmdline_wide_len);
      // Recalculate cmdline_bytes to the actual size (including null).
      cmdline_bytes =
          static_cast<SIZE_T>(raw_cmdline_wide_len) * sizeof(WCHAR);
    } else {
      build_cmdline(argv, wcmdline);
    }
    cursor += cmdline_bytes;
  }

  cursor = align_to(cursor, sizeof(WCHAR));

  // Wide environment block.
  WCHAR *wenv = nullptr;
  if (env_bytes) {
    wenv = reinterpret_cast<WCHAR *>(cursor);
    build_env_block(envp, wenv);
    cursor += env_bytes;
  }

  // fd_handles array, indexed by fd number.
  cursor = align_to(cursor, alignof(HANDLE));
  HANDLE *fd_handle_buf = reinterpret_cast<HANDLE *>(cursor);
  cursor += fd_handles_bytes;

  cursor = align_to(cursor, alignof(uintptr_t));
  uintptr_t *pty_key_buf = reinterpret_cast<uintptr_t *>(cursor);
  cursor += pty_keys_bytes;

  // --- Phase 3: Apply file actions ---
  FileActionState fa_state;
  fa_state.init(fd_handle_buf, pty_key_buf, fa_max_fd);
  auto cleanup_fa = cpp::make_scope_guard([&] { fa_state.cleanup(); });
  int fa_err = apply_file_actions(file_actions, fa_state);
  if (fa_err) {
    return -fa_err;
  }

  // --- Phase 4: Create stop/continue notification objects ---
  //
  // Anonymous section (ChildStateBlock) + manual-reset event for the child
  // to signal stop/continue. Both are inherited by the child and passed
  // via lpReserved2 extension.
  NTSTATUS sec_st;
  windows::SectionHandle state_section =
      windows::SectionHandle::create_anon_rw(
          sizeof(signal_state::ChildStateBlock), &sec_st);
  HANDLE state_event = nullptr;
  {
    // Anonymous manual-reset event: nullptr ObjectAttributes = no name.
    ::NtCreateEvent(&state_event, EVENT_MODIFY_STATE | SYNCHRONIZE, nullptr,
                    NotificationEvent,
                    /*InitialState=*/FALSE);
  }

  // Mark both as inheritable.
  if (state_section)
    (void)mark_handle_inheritable(state_section.get());
  if (state_event)
    (void)mark_handle_inheritable(state_event);

  pty_tree::CurrentAttachment inherited_attachment = {};
  if (!fa_state.has_pty_spawn) {
    int attachment_err =
        pty_tree::duplicate_current_attachment(&inherited_attachment, true);
    if (attachment_err != 0)
      return -attachment_err;
  }
  auto cleanup_attachment = cpp::make_scope_guard([&] {
    if (!fa_state.has_pty_spawn)
      pty_tree::release_current_attachment(&inherited_attachment);
  });

  // --- Phase 5: Set up secure handle inheritance ---

  // Collect all unique valid handles for the native handle list.
  HANDLE *inherit_handles = reinterpret_cast<HANDLE *>(cursor);
  cursor += (static_cast<SIZE_T>(fa_max_fd) + 4) * sizeof(HANDLE);
  DWORD inherit_count = 0;
  for (int i = 0; i < fa_state.max_fd; ++i) {
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        fa_state.fd_handles[i]);
  }
  process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                      state_event);
  process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                      state_section.get());
  process_utils::append_unique_handle(
      inherit_handles, &inherit_count,
      NtCurrentPeb()->ProcessParameters->CurrentDirectory.Handle);
  HANDLE explicit_console = nullptr;
  if (fa_state.has_pty_spawn) {
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        fa_state.pty_spawn.reference);
  } else {
    explicit_console = console::current_reference();
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        explicit_console);
  }
  if (console_util::is_live_console_handle(explicit_console)) {
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        explicit_console);
  }
  if (fa_state.has_pty_spawn) {
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        fa_state.pty_spawn.state_lock);
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        fa_state.pty_spawn.state_section);
  } else {
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        inherited_attachment.state_lock);
    process_utils::append_unique_handle(inherit_handles, &inherit_count,
                                        inherited_attachment.state_section);
  }

  // Build lpReserved2 block (MSVC CRT protocol) for fd inheritance.
  cursor = align_to(cursor, 8);
  BYTE *reserved2 = reinterpret_cast<BYTE *>(cursor);
  build_reserved2(reserved2, fa_state.fd_handles, fa_state.max_fd);

  // Append llvm-libc extension with state handles and spawn attributes.
  {
    SIZE_T std_size = reserved2_size(fa_state.max_fd);
    SIZE_T aligned_offset = (std_size + 7) & ~SIZE_T{7};
    auto *r2ext = reinterpret_cast<signal_state::Reserved2Ext *>(
        reserved2 + aligned_offset);
    r2ext->magic = signal_state::LLVM_LIBC_RESERVED2_MAGIC;
    r2ext->event = state_event;
    r2ext->section = state_section.get();
    r2ext->attr_flags = static_cast<DWORD>(attr_flags);
    r2ext->sigmask = sigmask_bits;
    r2ext->sigdefault = sigdefault_bits;

    auto *pty_ext = reinterpret_cast<PtyReserved2Ext *>(
        reserved2 + aligned_offset + signal_state::RESERVED2_EXT_SIZE);
    pty_ext->magic = LLVM_LIBC_PTY_RESERVED2_MAGIC;
    pty_ext->version = LLVM_LIBC_PTY_RESERVED2_VERSION;
    pty_ext->flags = fa_state.has_pty_spawn ? fa_state.pty_stdio_mask : 0;
    pty_tree::copy_tree_nonce(pty_ext->tree_nonce);
    pty_ext->attached_pty_id = fa_state.has_pty_spawn
                                   ? fa_state.pty_spawn.pty_id
                                   : pty_tree::current_attached_pty_id();
    pty_ext->reserved0 = 0;
    pty_ext->attached_pty_reference =
        fa_state.has_pty_spawn ? fa_state.pty_spawn.reference : nullptr;
    pty_ext->attached_pty_state_lock = fa_state.has_pty_spawn
                                           ? fa_state.pty_spawn.state_lock
                                           : inherited_attachment.state_lock;
    pty_ext->attached_pty_state_section =
        fa_state.has_pty_spawn ? fa_state.pty_spawn.state_section
                               : inherited_attachment.state_section;
  }

  // --- Phase 6: NtCreateUserProcess ---
  auto nt_path_s = path_scratch();
  if (!nt_path_s) return -ENOMEM;
  WCHAR *nt_path = nt_path_s.data();
  size_t nt_path_len = to_nt_path(path, nt_path, nt_path_s.size());
  if (nt_path_len == 0) {
    return -EINVAL;
  }

  // POSIX_SPAWN_SETSCHEDULER / POSIX_SPAWN_SETSCHEDPARAM: set scheduling
  // policy and priority. On Windows, both map to a process priority class.
  bool has_priority_class = false;
  UCHAR priority_class = PROCESS_PRIORITY_CLASS_NORMAL;
  if (attr &&
      (attr_flags & (POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSCHEDPARAM))) {
    int policy = (attr_flags & POSIX_SPAWN_SETSCHEDULER) ? attr->__schedpolicy
                                                         : SCHED_OTHER;
    int prio = attr->__schedparam.sched_priority;
    has_priority_class = true;

    if (policy == SCHED_FIFO || policy == SCHED_RR) {
      priority_class = PROCESS_PRIORITY_CLASS_REALTIME;
    } else {
      if (prio <= -20)
        priority_class = PROCESS_PRIORITY_CLASS_IDLE;
      else if (prio < 0)
        priority_class = PROCESS_PRIORITY_CLASS_BELOW_NORMAL;
      else if (prio == 0)
        priority_class = PROCESS_PRIORITY_CLASS_NORMAL;
      else if (prio < 20)
        priority_class = PROCESS_PRIORITY_CLASS_ABOVE_NORMAL;
      else
        priority_class = PROCESS_PRIORITY_CLASS_HIGH;
    }
  }

  // Guard for state event — dismissed when transferred to child table.
  // state_section is RAII (SectionHandle) — no scope guard needed.
  auto close_state_event = cpp::make_scope_guard([&] {
    if (state_event)
      ::NtClose(state_event);
  });

  // POSIX_SPAWN_RESETIDS: de-elevate the child if running as elevated admin.
  HANDLE delevated_token = nullptr;
  if (attr_flags & POSIX_SPAWN_RESETIDS) {
    int err = get_delevated_token(&delevated_token);
    if (err)
      return -err;
  }

  // Propagate the effective identity if seteuid/setuid changed it.
  // Without this, the child would inherit the process token (the real
  // identity), not the impersonated effective identity.
  HANDLE identity_token = nullptr;
  if (!delevated_token) {
    identity_token = windows_identity::get_child_primary_token();
  }

  HANDLE launch_token = delevated_token ? delevated_token : identity_token;
  process_utils::NativeProcessLaunchOptions launch_opts;
  launch_opts.image_path = nt_path_len > 4 ? nt_path + 4 : wpath;
  launch_opts.image_path_chars = nt_path_len > 4
                                     ? nt_path_len - 4
                                     : static_cast<SIZE_T>(path_wide_len - 1);
  launch_opts.nt_image_path = nt_path;
  launch_opts.nt_image_path_chars = nt_path_len;
  launch_opts.command_line = wcmdline;
  launch_opts.command_line_bytes = cmdline_bytes;
  launch_opts.environment = wenv;
  launch_opts.inherit_handles = inherit_handles;
  launch_opts.inherit_handle_count = inherit_count;
  launch_opts.std_in = fa_state.fd_handles[0];
  launch_opts.std_out = fa_state.fd_handles[1];
  launch_opts.std_err = fa_state.fd_handles[2];
  launch_opts.console_handle = explicit_console;
  launch_opts.runtime_data = reserved2;
  launch_opts.runtime_data_size = reserved2_bytes;
  launch_opts.token = launch_token;
  launch_opts.has_priority_class = has_priority_class;
  launch_opts.priority_class = priority_class;
  launch_opts.set_process_group = (attr_flags & POSIX_SPAWN_SETPGROUP) != 0;
  launch_opts.process_group_id =
      (attr && attr->__pgroup > 0) ? static_cast<DWORD>(attr->__pgroup) : 0;
  launch_opts.resume_immediately = !fa_state.has_pty_spawn;

  process_utils::NativeProcessLaunchResult launch_result;
  int launch_err =
      process_utils::launch_user_process_native(launch_opts, &launch_result);
  if (delevated_token)
    NtClose(delevated_token);
  if (identity_token)
    NtClose(identity_token);
  // --- Shebang fallback: if ENOEXEC, parse #! and retry with interpreter ---
  if (launch_err == ENOEXEC) {
    char interp[SHEBANG_MAX_PATH];
    char interp_arg[SHEBANG_MAX_PATH];
    int interp_len = 0, arg_len = 0;

    if (parse_shebang(nt_path, nt_path_len, interp, &interp_len,
                      interp_arg, &arg_len) == 0) {
      // Build new argv: [interpreter, optional_arg, script_path, argv[1:]]
      int orig_argc = 0;
      if (argv) {
        for (int i = 0; argv[i]; ++i)
          ++orig_argc;
      }
      int new_argc = 1 + (arg_len > 0 ? 1 : 0) + 1 +
                     (orig_argc > 1 ? orig_argc - 1 : 0);
      constexpr int MAX_SHEBANG_ARGV = 256;
      if (new_argc >= MAX_SHEBANG_ARGV)
        return -E2BIG;

      char *new_argv[MAX_SHEBANG_ARGV];
      int pos = 0;
      new_argv[pos++] = interp;
      if (arg_len > 0)
        new_argv[pos++] = interp_arg;
      new_argv[pos++] = const_cast<char *>(path); // Script path.
      if (argv) {
        for (int i = 1; i < orig_argc; ++i)
          new_argv[pos++] = argv[i];
      }
      new_argv[pos] = nullptr;

      // Recurse with the interpreter as the image path.
      return internal::posix_spawn(pid_out, interp, file_actions, attr,
                                   new_argv, envp);
    }
  }
  if (launch_err)
    return -launch_err;

  // Process created — guard to terminate + close on post-create failures.
  auto kill_child = cpp::make_scope_guard([&] {
    NtTerminateProcess(launch_result.process, 127);
    ::NtClose(launch_result.process);
    if (launch_result.thread)
      ::NtClose(launch_result.thread);
  });

  // --- Phase 7: Register child in tracking table ---
  // State section is moved to the child table; event ownership transfers too.
  // Determine the child's initial process group:
  //   POSIX_SPAWN_SETPGROUP with pgroup > 0: explicit group
  //   POSIX_SPAWN_SETPGROUP with pgroup == 0: child becomes group leader
  //   No POSIX_SPAWN_SETPGROUP: inherit parent's pgid
  pid_t child_pgid;
  if (launch_opts.set_process_group) {
    child_pgid = (launch_opts.process_group_id > 0)
                     ? static_cast<pid_t>(launch_opts.process_group_id)
                     : static_cast<pid_t>(launch_result.process_id);
  } else {
    child_pgid = process::get_self_pgid();
  }

  pid_t child_sid = windows_syscalls::get_session_id();

  if (fa_state.has_pty_spawn) {
    int seed_err = pty_tree::seed_initial_session(
        fa_state.pty_spawn.state_lock, fa_state.pty_spawn.state_section, child_sid,
        child_pgid);
    if (seed_err)
      return -seed_err;

    NTSTATUS resume_status = ::NtAlertResumeThread(launch_result.thread, nullptr);
    if (!NT_SUCCESS(resume_status)) {
      return -windows_util::ntstatus_to_errno(resume_status);
    }
  }

  if (launch_result.thread) {
    ::NtClose(launch_result.thread);
    launch_result.thread = nullptr;
  }

  int track_err = process::track_child(launch_result.process,
                                       launch_result.process_id, child_pgid,
                                       state_event, cpp::move(state_section));
  if (track_err)
    return -track_err;

  // All resources committed — dismiss guards.
  kill_child.dismiss();
  close_state_event.dismiss();

  if (pid_out)
    *pid_out = static_cast<pid_t>(launch_result.process_id);

  return 0;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
