//===-- VT-backed PTY session support ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/new.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/io/string_utils.h"
#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_file.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "src/__support/OSUtil/windows/process/console_handle_utils.h"
#include "src/__support/OSUtil/windows/process/console_tty.h"
#include "src/__support/OSUtil/windows/process/nt_process_utils.h"
#include "src/__support/OSUtil/windows/process/process_utils.h"
#include "src/__support/OSUtil/windows/process/pty_reserved2.h"
#include "src/__support/OSUtil/windows/process/pty_shared_state.h"
#include "src/__support/OSUtil/windows/process/terminal_foreground.h"
#include "src/__support/OSUtil/windows/process/pty_tree.h"
#include "src/__support/OSUtil/windows/process/termios_defaults.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/threads/raw_mutex.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace vt_pty {
struct Session {
  cpp::Atomic<uint32_t> refcount;
  RawMutex lock;
  console::Session slave_console;
  windows::ScopedNtHandle master_input;
  windows::ScopedNtHandle master_output;
  windows::ScopedNtHandle slave_output_pipe;
  windows::ScopedNtHandle signal_write;
  windows::ScopedNtHandle conhost_process;
  HANDLE shared_state_lock;
  HANDLE shared_state_section;
  PtySharedState *shared_state;
  uint32_t id;
  bool owns_tree_registration;

  Session()
      : refcount(1), lock(), slave_console(),
        master_input(), master_output(), slave_output_pipe(), signal_write(),
        conhost_process(),
        shared_state_lock(nullptr), shared_state_section(nullptr),
        shared_state(nullptr), id(0), owns_tree_registration(false) {}
};

namespace {

constexpr uint16_t DEFAULT_ROWS = 25;
constexpr uint16_t DEFAULT_COLS = 80;
constexpr DWORD MAX_PIPE_IO = 0xFFFFFFFFu;
// Give a freshly launched conhost up to 5 seconds to publish the ConDrv
// connect endpoint before treating startup as failed.
constexpr int CONNECT_RETRY_INTERVAL_MS = 10;
constexpr int CONNECT_RETRY_TIMEOUT_MS = 5000;
constexpr int64_t HUNDRED_NS_PER_MILLISECOND = 10000LL;
constexpr int MAX_CONNECT_RETRIES =
    CONNECT_RETRY_TIMEOUT_MS / CONNECT_RETRY_INTERVAL_MS;
constexpr int64_t CONNECT_RETRY_INTERVAL_100NS =
    -static_cast<int64_t>(CONNECT_RETRY_INTERVAL_MS) *
    HUNDRED_NS_PER_MILLISECOND;
constexpr char PTMX_PATH[] = "/dev/ptmx";
constexpr char PTS_PREFIX[] = "/dev/pts/";

RawMutex current_attachment_lock;
Session *current_attachment_keepalive = nullptr;

OpenFileDescription *get_ofd(int fd) {
  return fd_table.get_ofd(fd);
}

Session *session_from_ofd(OpenFileDescription *ofd) {
  if (!ofd || (!ofd->is_pty_master() && !ofd->is_pty_slave()))
    return nullptr;
  LIBC_ASSERT((ofd->kind == FileKind::PtyMaster ||
               ofd->kind == FileKind::PtySlave) &&
              "pty_session() requires PTY kind");
  return reinterpret_cast<Session *>(ofd->pty_session());
}

void set_current_attachment_keepalive(Session *session) {
  current_attachment_lock.lock();
  Session *old = current_attachment_keepalive;
  if (session)
    retain(session);
  current_attachment_keepalive = session;
  current_attachment_lock.unlock();
  release(old);
}

struct ScopedAttachedPtyId {
  pty_tree::CurrentAttachment previous = {};
  bool active = false;

  explicit ScopedAttachedPtyId(Session *session) {
    if (!session)
      return;

    (void)pty_tree::duplicate_current_attachment(&previous);

    pty_tree::CurrentAttachment current = {};
    current.pty_id = session->id;
    current.state_lock = session->shared_state_lock;
    current.state_section = session->shared_state_section;
    if (pty_tree::set_current_attached_pty(current) == 0)
      active = true;
  }

  ~ScopedAttachedPtyId() {
    if (active) {
      if (previous.pty_id != 0)
        (void)pty_tree::set_current_attached_pty(previous);
      else
        pty_tree::clear_current_attached_pty();
    }
    pty_tree::release_current_attachment(&previous);
  }
};

size_t decimal_length(uint32_t value) {
  size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

size_t pts_path_length(uint32_t id) {
  return sizeof(PTS_PREFIX) - 1 + decimal_length(id);
}

int copy_pts_path(char *buffer, size_t size, uint32_t id) {
  if (!buffer)
    return ERANGE;

  size_t needed = pts_path_length(id) + 1;
  if (size < needed)
    return ERANGE;

  size_t pos = 0;
  for (size_t i = 0; i < sizeof(PTS_PREFIX) - 1; ++i)
    buffer[pos++] = PTS_PREFIX[i];

  uint32_t divisor = 1000000000u;
  bool started = false;
  while (divisor != 0) {
    uint32_t digit = id / divisor;
    if (digit != 0 || started || divisor == 1) {
      started = true;
      buffer[pos++] = static_cast<char>('0' + digit);
    }
    id %= divisor;
    divisor /= 10;
  }
  buffer[pos] = '\0';
  return 0;
}

bool parse_pts_id(const char *path, uint32_t *id_out) {
  if (!string_util::starts_with(path, PTS_PREFIX))
    return false;

  const char *digits = path + sizeof(PTS_PREFIX) - 1;
  if (*digits == '\0')
    return false;

  uint32_t value = 0;
  while (*digits) {
    if (*digits < '0' || *digits > '9')
      return false;
    uint32_t next = value * 10 + static_cast<uint32_t>(*digits - '0');
    if (next < value)
      return false;
    value = next;
    ++digits;
  }

  if (value == 0)
    return false;
  if (id_out)
    *id_out = value;
  return true;
}

int normalize_master_open_flags(int flags) {
  int accmode = flags & O_ACCMODE;
  if (accmode != O_RDWR)
    return -EINVAL;

  int invalid = flags & ~(O_ACCMODE | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (invalid != 0)
    return -EINVAL;

  return (flags & (O_CLOEXEC | O_NONBLOCK)) | O_RDWR;
}

int normalize_slave_open_flags(int flags) {
  int accmode = flags & O_ACCMODE;
  if (accmode != O_RDONLY && accmode != O_WRONLY && accmode != O_RDWR)
    return -EINVAL;

  int invalid =
      flags & ~(O_ACCMODE | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (invalid != 0)
    return -EINVAL;

  return (flags & (O_CLOEXEC | O_NONBLOCK)) | accmode;
}

void init_default_winsize(struct winsize *ws) {
  ws->ws_row = DEFAULT_ROWS;
  ws->ws_col = DEFAULT_COLS;
  ws->ws_xpixel = 0;
  ws->ws_ypixel = 0;
}

void maybe_seed_current_winsize(struct winsize *ws) {
  HANDLE raw = nullptr;
  NTSTATUS status = condrv::open_condrv_absolute(
      &raw, condrv::CONDRV_CURRENT_OUTPUT_PATH,
      FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE);
  if (!NT_SUCCESS(status))
    return;
  windows::ScopedNtHandle handle(raw);

  // Headless ConPTY sessions report row/col through the screen-buffer query
  // path; pixel/display ioctls are best-effort and may be unsupported.
  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  status = condrv::get_console_screen_buffer_info_ex(handle.get(), &info);
  if (!NT_SUCCESS(status))
    return;

  console_util::srwindow_to_rowcol(info.srWindow, &ws->ws_row, &ws->ws_col);
}

int lock_shared_state(Session *session) {
  if (!session || !session->shared_state || !session->shared_state_lock)
    return -ENOTTY;
  NTSTATUS status = ::NtWaitForSingleObject(session->shared_state_lock, FALSE,
                                            nullptr);
  if (status == STATUS_ABANDONED)
    return 0;
  if (status == STATUS_INVALID_HANDLE)
    return -EBADF;
  if (!NT_SUCCESS(status))
    return -EIO;
  return 0;
}

void unlock_shared_state(Session *session) {
  if (session && session->shared_state_lock)
    (void)::NtReleaseMutant(session->shared_state_lock, nullptr);
}

template <typename Fn> auto with_shared_state(Session *session, Fn &&fn) {
  int err = lock_shared_state(session);
  if (err < 0)
    return err;
  auto result = fn(*session->shared_state);
  unlock_shared_state(session);
  return result;
}

// Write variant: wraps the mutation with seqlock begin/end so that
// lock-free readers (slave processes with PAGE_READONLY mappings)
// can detect torn writes and retry.
template <typename Fn> auto with_shared_state_write(Session *session, Fn &&fn) {
  int err = lock_shared_state(session);
  if (err < 0)
    return err;
  __atomic_store_n(&session->shared_state->change_seq,
                   __atomic_load_n(&session->shared_state->change_seq,
                                   __ATOMIC_RELAXED) + 1,
                   __ATOMIC_RELEASE);
  auto result = fn(*session->shared_state);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&session->shared_state->change_seq,
                   __atomic_load_n(&session->shared_state->change_seq,
                                   __ATOMIC_RELAXED) + 1,
                   __ATOMIC_RELEASE);
  unlock_shared_state(session);
  return result;
}

int apply_termios_to_session(Session *session) {
  if (!session || !session->shared_state || !session->slave_console.handles.Input ||
      !session->slave_console.handles.Output)
    return -EINVAL;

  struct termios attrs = {};
  int err = with_shared_state(session, [&](const PtySharedState &shared) {
    attrs = shared.attrs;
    return 0;
  });
  if (err < 0)
    return err;

  NTSTATUS status =
      session->slave_console.handles.Connection
          ? condrv::set_console_mode_on(
                session->slave_console.handles.Connection,
                session->slave_console.handles.Input, termios_defaults::desired_input_mode(attrs))
          : condrv::set_console_mode(session->slave_console.handles.Input,
                                     termios_defaults::desired_input_mode(attrs));
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  status = session->slave_console.handles.Connection
               ? condrv::set_console_mode_on(
                     session->slave_console.handles.Connection,
                     session->slave_console.handles.Output,
                     termios_defaults::desired_output_mode(attrs))
               : condrv::set_console_mode(session->slave_console.handles.Output,
                                          termios_defaults::desired_output_mode(attrs));
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  return 0;
}

NTSTATUS get_session_screen_buffer_info(
    Session *session, condrv::CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!session || !info || !session->slave_console.handles.Output)
    return STATUS_INVALID_HANDLE;

  HANDLE connection = session->slave_console.handles.Connection;
  if (connection) {
    return condrv::get_console_screen_buffer_info_ex_on(
        connection, session->slave_console.handles.Output, info);
  }
  return condrv::get_console_screen_buffer_info_ex(
      session->slave_console.handles.Output, info);
}

NTSTATUS set_session_screen_buffer_info(
    Session *session, const condrv::CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!session || !info || !session->slave_console.handles.Output)
    return STATUS_INVALID_HANDLE;

  HANDLE connection = session->slave_console.handles.Connection;
  if (connection) {
    return condrv::set_console_screen_buffer_info_ex_wrapper_on(
        connection, session->slave_console.handles.Output, info);
  }
  return condrv::set_console_screen_buffer_info_ex_wrapper(
      session->slave_console.handles.Output, info);
}

int apply_winsize_to_session(Session *session,
                                         const struct winsize &ws) {
  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  NTSTATUS status = get_session_screen_buffer_info(session, &info);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  const SHORT cols = static_cast<SHORT>(ws.ws_col);
  const SHORT rows = static_cast<SHORT>(ws.ws_row);
  const long right_exclusive = static_cast<long>(info.srWindow.Left) + cols;
  const long bottom_exclusive = static_cast<long>(info.srWindow.Top) + rows;
  if (right_exclusive > 0x7FFF || bottom_exclusive > 0x7FFF)
    return -EINVAL;

  info.dwSize.X = cols;
  if (rows > info.dwSize.Y)
    info.dwSize.Y = rows;
  info.srWindow.Right = static_cast<SHORT>(right_exclusive);
  info.srWindow.Bottom = static_cast<SHORT>(bottom_exclusive);

  status = set_session_screen_buffer_info(session, &info);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);

  return 0;
}

bool deliver_controller_winsize_signal(pid_t controller_pid,
                                                   uint64_t controller_create_time) {
  if (controller_pid <= 0)
    return false;

  const pid_t self_pid = static_cast<pid_t>(NtCurrentProcessId());
  if (controller_pid == self_pid) {
    uint64_t self_create_time = process_util::current_process_create_time();
    if (controller_create_time != 0 && self_create_time != 0 &&
        controller_create_time != self_create_time)
      return false;
    signal_state::deliver_process_signal(SIGWINCH);
    return true;
  }

  HANDLE process = nullptr;
  NTSTATUS status = ::NtOpenProcessById(
      &process, PROCESS_QUERY_LIMITED_INFORMATION,
      static_cast<DWORD>(controller_pid));
  if (!NT_SUCCESS(status))
    return false;

  uint64_t live_create_time = process_util::query_process_create_time(process);
  ::NtClose(process);
  if (live_create_time == 0)
    return false;
  if (controller_create_time != 0 && controller_create_time != live_create_time)
    return false;

  intptr_t rc = signal_state::kill(static_cast<intptr_t>(controller_pid),
                                   SIGWINCH);
  return rc == 0 || rc != -ESRCH;
}

void deliver_session_winsize_signal(pid_t foreground_pgrp,
                                                pid_t controller_pid,
                                                uint64_t controller_create_time) {
  // A master-side resize often runs in a different process from the PTY
  // controller. Until tree-wide foreground-group authority lands, prefer the
  // recorded controller over parent-local process-group bookkeeping so we
  // don't treat self-only delivery as success.
  if (controller_pid > 0 &&
      controller_pid != static_cast<pid_t>(NtCurrentProcessId()) &&
      deliver_controller_winsize_signal(controller_pid,
                                        controller_create_time))
    return;

  if (foreground_pgrp > 0) {
    intptr_t rc =
        signal_state::kill(-static_cast<intptr_t>(foreground_pgrp), SIGWINCH);
    if (rc == 0 || rc != -ESRCH)
      return;
  }

  (void)deliver_controller_winsize_signal(controller_pid,
                                          controller_create_time);
}

using console_util::duplicate_noninherited;
using console_util::duplicate_inherited;
using console_util::append_hex_uintptr;
using console_util::append_uint16;
using process_util::process_has_exited;

bool is_connect_retry_status(NTSTATUS status) {
  return status == STATUS_OBJECT_NAME_NOT_FOUND ||
         status == STATUS_OBJECT_PATH_NOT_FOUND;
}

[[maybe_unused]] NTSTATUS
wait_for_condrv_connect(HANDLE *out, HANDLE reference, HANDLE conhost_process);

[[maybe_unused]] NTSTATUS rebuild_local_client_handles_from_reference(
    console::Session *session) {
  if (!session || !session->reference)
    return STATUS_INVALID_PARAMETER;

  HANDLE connection = nullptr;
  NTSTATUS status =
      wait_for_condrv_connect(&connection, session->reference, nullptr);
  if (!NT_SUCCESS(status))
    return status;

  condrv::CONDRV_CLIENT_HANDLES rebuilt_handles = {};
  status = condrv::open_condrv_client_handles(&rebuilt_handles, connection,
                                              false);
  if (!NT_SUCCESS(status)) {
    ::NtClose(connection);
    return status;
  }

  condrv::close_condrv_client_handles(&session->handles);
  session->handles = rebuilt_handles;
  return STATUS_SUCCESS;
}

NTSTATUS rebuild_local_client_handles_from_connection(
    console::Session *session, HANDLE connection) {
  if (!session || !connection)
    return STATUS_INVALID_PARAMETER;

  condrv::CONDRV_CLIENT_HANDLES rebuilt_handles = {};
  NTSTATUS status =
      condrv::open_condrv_client_handles(&rebuilt_handles, connection, false);
  if (!NT_SUCCESS(status)) {
    ::NtClose(connection);
    return status;
  }

  condrv::close_condrv_client_handles(&session->handles);
  session->handles = rebuilt_handles;
  return STATUS_SUCCESS;
}

[[maybe_unused]] NTSTATUS adopt_inherited_client_handles_from_reference(
    console::Session *session) {
  return rebuild_local_client_handles_from_reference(session);
}

[[maybe_unused]] NTSTATUS
wait_for_condrv_connect(HANDLE *out, HANDLE reference, HANDLE conhost_process) {
  if (!out || !reference)
    return STATUS_INVALID_PARAMETER;

  for (int attempt = 0; attempt != MAX_CONNECT_RETRIES; ++attempt) {
    NTSTATUS status =
        condrv::open_condrv_child(out, reference, condrv::CONDRV_CONNECT_NAME);
    if (NT_SUCCESS(status))
      return status;
    if (!is_connect_retry_status(status))
      return status;
    if (process_has_exited(conhost_process))
      return STATUS_UNSUCCESSFUL;

    LARGE_INTEGER interval = {};
    interval.QuadPart = CONNECT_RETRY_INTERVAL_100NS;
    status = ::NtDelayExecution(FALSE, &interval);
    if (!NT_SUCCESS(status))
      return status;
  }

  if (process_has_exited(conhost_process))
    return STATUS_UNSUCCESSFUL;
  return STATUS_OBJECT_NAME_NOT_FOUND;
}

[[maybe_unused]] NTSTATUS
wait_create_condrv_connect(HANDLE *out, HANDLE root,
                           const condrv::CONSOLE_SERVER_MSG &server_msg,
                           HANDLE conhost_process) {
  if (!out || !root)
    return STATUS_INVALID_PARAMETER;

  for (int attempt = 0; attempt != MAX_CONNECT_RETRIES; ++attempt) {
    NTSTATUS status =
        condrv::create_condrv_connect(out, root, server_msg, false);
    if (NT_SUCCESS(status))
      return status;
    if (!is_connect_retry_status(status))
      return status;
    if (process_has_exited(conhost_process))
      return STATUS_UNSUCCESSFUL;

    LARGE_INTEGER interval = {};
    interval.QuadPart = CONNECT_RETRY_INTERVAL_100NS;
    status = ::NtDelayExecution(FALSE, &interval);
    if (!NT_SUCCESS(status))
      return status;
  }

  if (process_has_exited(conhost_process))
    return STATUS_UNSUCCESSFUL;
  return STATUS_OBJECT_NAME_NOT_FOUND;
}

int launch_headless_conhost(const UNICODE_STRING *nt_image_path,
                            const UNICODE_STRING *command_line,
                            HANDLE *inherit_handles,
                            DWORD inherit_handle_count, HANDLE std_in,
                            HANDLE std_out, HANDLE std_err,
                            HANDLE *process_out) {
  if (!nt_image_path || !nt_image_path->Buffer || !command_line ||
      !command_line->Buffer || !inherit_handles || !inherit_handle_count ||
      !process_out)
    return EINVAL;

  const WCHAR *image_path = nt_image_path->Buffer;
  SIZE_T image_path_chars = nt_image_path->Length / sizeof(WCHAR);
  if (image_path_chars <= 4 || image_path[0] != u'\\' || image_path[1] != u'?' ||
      image_path[2] != u'?' || image_path[3] != u'\\')
    return ENOENT;

  auto *params = NtCurrentPeb()->ProcessParameters;
  process_utils::NativeProcessLaunchOptions launch_opts;
  launch_opts.image_path = image_path + 4;
  launch_opts.image_path_chars = image_path_chars - 4;
  launch_opts.nt_image_path = nt_image_path->Buffer;
  launch_opts.nt_image_path_chars = image_path_chars;
  launch_opts.command_line = command_line->Buffer;
  launch_opts.command_line_bytes =
      static_cast<SIZE_T>(command_line->Length) + sizeof(WCHAR);
  launch_opts.environment = params ? params->Environment : nullptr;
  launch_opts.inherit_handles = inherit_handles;
  launch_opts.inherit_handle_count = inherit_handle_count;
  launch_opts.std_in = std_in;
  launch_opts.std_out = std_out;
  launch_opts.std_err = std_err;
  launch_opts.console_handle =
      reinterpret_cast<HANDLE>(console_util::CONSOLE_HANDLE_NO_WINDOW);
  launch_opts.handles_pre_inherited = true;

  process_utils::NativeProcessLaunchResult launch_result;
  int launch_err =
      process_utils::launch_user_process_native(launch_opts, &launch_result);
  if (launch_err != 0)
    return launch_err;

  console_util::close_handle_if_valid(&launch_result.thread);
  *process_out = launch_result.process;
  return 0;
}

NTSTATUS create_inheritable_pipe_pair(HANDLE *read_end_out,
                                      HANDLE *write_end_out) {
  if (!read_end_out || !write_end_out)
    return STATUS_INVALID_PARAMETER;

  *read_end_out = nullptr;
  *write_end_out = nullptr;

  static constexpr WCHAR npfs_root[] = u"\\Device\\NamedPipe\\";
  UNICODE_STRING npfs_name;
  npfs_name.Length = sizeof(npfs_root) - sizeof(WCHAR);
  npfs_name.MaximumLength = npfs_name.Length;
  npfs_name.Buffer = const_cast<WCHAR *>(npfs_root);

  auto root_oa = windows::named_internal_oa(&npfs_name);
  IO_STATUS_BLOCK iosb = {};

  HANDLE raw_npfs_dir = nullptr;
  NTSTATUS status = ::NtOpenFile(
      &raw_npfs_dir, SYNCHRONIZE | FILE_READ_ATTRIBUTES, &root_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle npfs_dir(raw_npfs_dir);

  UNICODE_STRING empty_name = {};
  auto pipe_oa = windows::named_oa(&empty_name, npfs_dir.get(), true);

  LARGE_INTEGER timeout = {};
  timeout.QuadPart = -1200000000LL; // 120 seconds (default, not actually used)

  HANDLE raw_read_end = nullptr;
  iosb = {};
  status = ::NtCreateNamedPipeFile(
      &raw_read_end, FILE_GENERIC_READ | FILE_WRITE_ATTRIBUTES, &pipe_oa,
      &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE, 0,
      FILE_PIPE_MESSAGE_TYPE, FILE_PIPE_BYTE_STREAM_MODE,
      FILE_PIPE_QUEUE_OPERATION, 1, 65536, 65536, &timeout);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle read_end(raw_read_end);

  pipe_oa.RootDirectory = read_end.get();

  HANDLE raw_write_end = nullptr;
  iosb = {};
  status = ::NtOpenFile(&raw_write_end,
                        FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES, &pipe_oa,
                        &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle write_end(raw_write_end);

  *read_end_out = read_end.release();
  *write_end_out = write_end.release();
  return STATUS_SUCCESS;
}

NTSTATUS create_pipe_endpair(HANDLE *parent_end, HANDLE *child_end,
                                         bool parent_reads) {
  if (!parent_end || !child_end)
    return STATUS_INVALID_PARAMETER;

  *parent_end = nullptr;
  *child_end = nullptr;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  NTSTATUS status = create_inheritable_pipe_pair(&read_end, &write_end);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle read_end_guard(read_end);
  windows::ScopedNtHandle write_end_guard(write_end);

  windows::ScopedNtHandle *inherited_parent =
      parent_reads ? &read_end_guard : &write_end_guard;
  windows::ScopedNtHandle *inherited_child =
      parent_reads ? &write_end_guard : &read_end_guard;

  status = duplicate_noninherited(inherited_parent->get(), parent_end);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  ::NtClose(inherited_parent->release());
  *child_end = inherited_child->release();
  return STATUS_SUCCESS;
}

NTSTATUS duplicate_spawn_session_handles(const Session *session,
                                                     SpawnHandles *handles) {
  if (!session || !handles)
    return STATUS_INVALID_PARAMETER;

  *handles = {};
  handles->session_key = reinterpret_cast<uintptr_t>(session);
  handles->pty_id = session->id;

  NTSTATUS status = duplicate_inherited(session->slave_console.reference,
                                        &handles->reference);
  if (!NT_SUCCESS(status))
    return status;

  status = duplicate_inherited(session->shared_state_lock, &handles->state_lock);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->shared_state_section,
                               &handles->state_section);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->slave_console.handles.Connection,
                               &handles->connection);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->slave_console.handles.Input,
                               &handles->input);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->slave_console.handles.Output,
                               &handles->output);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->slave_console.handles.Error,
                               &handles->error);
  if (!NT_SUCCESS(status))
    goto fail;

  status = duplicate_inherited(session->slave_output_pipe.get(),
                               &handles->stdout_pipe);
  if (!NT_SUCCESS(status))
    goto fail;

  return STATUS_SUCCESS;

fail:
  console_util::close_handle_if_valid(&handles->state_section);
  console_util::close_handle_if_valid(&handles->state_lock);
  console_util::close_handle_if_valid(&handles->stdout_pipe);
  console_util::close_handle_if_valid(&handles->error);
  console_util::close_handle_if_valid(&handles->output);
  console_util::close_handle_if_valid(&handles->input);
  console_util::close_handle_if_valid(&handles->connection);
  console_util::close_handle_if_valid(&handles->reference);
  handles->pty_id = 0;
  handles->session_key = 0;
  return status;
}

void close_spawn_session_handles(SpawnHandles *handles) {
  if (!handles)
    return;
  console_util::close_handle_if_valid(&handles->state_section);
  console_util::close_handle_if_valid(&handles->state_lock);
  console_util::close_handle_if_valid(&handles->stdout_pipe);
  console_util::close_handle_if_valid(&handles->error);
  console_util::close_handle_if_valid(&handles->output);
  console_util::close_handle_if_valid(&handles->input);
  console_util::close_handle_if_valid(&handles->connection);
  console_util::close_handle_if_valid(&handles->reference);
  handles->pty_id = 0;
  handles->session_key = 0;
}

NTSTATUS
build_headless_command_line(WCHAR *buffer, size_t capacity,
                            const WCHAR *image_path, uint16_t cols,
                            uint16_t rows, HANDLE signal_handle,
                            HANDLE server_handle,
                            UNICODE_STRING *command_line) {
  if (!buffer || !command_line)
    return STATUS_INVALID_PARAMETER;

  size_t pos = 0;
  if (pos + 1 >= capacity)
    return STATUS_BUFFER_TOO_SMALL;
  buffer[pos++] = u'"';
  pos = console_util::append_wide(buffer, pos, capacity, image_path);
  if (pos + 1 >= capacity)
    return STATUS_BUFFER_TOO_SMALL;
  buffer[pos++] = u'"';
  pos = console_util::append_wide(buffer, pos, capacity, u" --headless --width ");
  pos = append_uint16(buffer, pos, capacity, cols);
  pos = console_util::append_wide(buffer, pos, capacity, u" --height ");
  pos = append_uint16(buffer, pos, capacity, rows);
  pos = console_util::append_wide(buffer, pos, capacity, u" --signal ");
  pos = append_hex_uintptr(buffer, pos, capacity,
                           reinterpret_cast<uintptr_t>(signal_handle));
  pos = console_util::append_wide(buffer, pos, capacity, u" --server ");
  pos = append_hex_uintptr(buffer, pos, capacity,
                           reinterpret_cast<uintptr_t>(server_handle));
  if (pos >= capacity)
    return STATUS_BUFFER_TOO_SMALL;
  buffer[pos] = 0;
  return ::RtlInitUnicodeStringEx(command_line, buffer);
}

ErrorOr<int> open_process_by_id(ULONG64 pid, HANDLE *process) {
  if (!process)
    return Error(EINVAL);
  *process = nullptr;

  CLIENT_ID cid = {};
  cid.UniqueProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid));
  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  NTSTATUS status =
      ::NtOpenProcess(process, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                      &oa, &cid);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));
  return 0;
}

int write_pipe_bytes(HANDLE handle, const void *buffer,
                                 size_t count) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  IO_STATUS_BLOCK iosb = {};
  ULONG io_len = count > MAX_PIPE_IO ? MAX_PIPE_IO : static_cast<ULONG>(count);
  NTSTATUS status =
      ::NtWriteFile(handle, nullptr, nullptr, nullptr, &iosb,
                    const_cast<void *>(buffer), io_len, nullptr, nullptr);
  if (!NT_SUCCESS(status))
    return -windows_util::ntstatus_to_errno(status);
  if (!NT_SUCCESS(iosb.Status))
    return -windows_util::ntstatus_to_errno(iosb.Status);
  return static_cast<int>(iosb.Information);
}

ssize_t read_pipe_bytes(HANDLE handle, void *buffer, size_t count,
                                    bool nonblocking) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  if (nonblocking) {
    IO_STATUS_BLOCK qiosb = {};
    FILE_PIPE_LOCAL_INFORMATION info = {};
    NTSTATUS qstatus = ::NtQueryInformationFile(
        handle, &qiosb, &info, sizeof(info), FilePipeLocalInformation);
    if (!NT_SUCCESS(qstatus))
      return -windows_util::ntstatus_to_errno(qstatus);
    if (info.ReadDataAvailable == 0)
      return -EAGAIN;
  }

  IO_STATUS_BLOCK iosb = {};
  ULONG io_len = count > MAX_PIPE_IO ? MAX_PIPE_IO : static_cast<ULONG>(count);
  NTSTATUS status = ::NtReadFile(handle, nullptr, nullptr, nullptr, &iosb,
                                 buffer, io_len, nullptr, nullptr);
  if (!NT_SUCCESS(status)) {
    if (status == STATUS_PIPE_BROKEN || status == STATUS_END_OF_FILE)
      return 0;
    return -windows_util::ntstatus_to_errno(status);
  }
  if (!NT_SUCCESS(iosb.Status)) {
    if (iosb.Status == STATUS_PIPE_BROKEN || iosb.Status == STATUS_END_OF_FILE)
      return 0;
    return -windows_util::ntstatus_to_errno(iosb.Status);
  }
  return static_cast<ssize_t>(iosb.Information);
}

ssize_t write_pipe_bytes(HANDLE handle, const void *buffer,
                                     size_t count, bool nonblocking) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  if (nonblocking) {
    IO_STATUS_BLOCK qiosb = {};
    FILE_PIPE_LOCAL_INFORMATION info = {};
    NTSTATUS qstatus = ::NtQueryInformationFile(
        handle, &qiosb, &info, sizeof(info), FilePipeLocalInformation);
    if (!NT_SUCCESS(qstatus))
      return -windows_util::ntstatus_to_errno(qstatus);
    if (info.WriteQuotaAvailable == 0)
      return -EAGAIN;
  }

  int written = write_pipe_bytes(handle, buffer, count);
  if (written < 0)
    return written;
  return static_cast<ssize_t>(written);
}

Session *allocate_session() {
  void *memory = page_alloc(sizeof(Session));
  if (!memory)
    return nullptr;
  return new (memory) Session();
}

void destroy_session(Session *session) {
  if (!session)
    return;
  if (session->shared_state) {
    if (session->owns_tree_registration) {
      int err = lock_shared_state(session);
      if (err == 0) {
        session->shared_state->flags |= PTY_SHARED_FLAG_HUNGUP;
        unlock_shared_state(session);
      }
      pty_tree::unregister_owned_pty(session->id);
    }
    pty_tree::close_shared_state(&session->shared_state_lock,
                                 &session->shared_state_section,
                                 &session->shared_state);
  }
  session->~Session();
  page_free(session);
}

int create_master_fd(Session *session, int flags) {
  auto result = fd_table.alloc(session->master_input.get(), flags, 0,
                               FileKind::PtyMaster);
  if (!result.has_value())
    return -result.error();

  if (flags & O_CLOEXEC)
    fd_table.set_fd_cloexec(result.value(), true);

  OpenFileDescription *ofd = get_ofd(result.value());
  if (!ofd)
    return -EIO;
  retain(session);
  ofd->set_pty_session(session);
  return result.value();
}

int create_slave_fd(Session *session, int flags) {
  auto result = fd_table.alloc(session->slave_console.handles.Input, flags, 0,
                               FileKind::PtySlave);
  if (!result.has_value())
    return -result.error();

  if (flags & O_CLOEXEC)
    fd_table.set_fd_cloexec(result.value(), true);

  OpenFileDescription *ofd = get_ofd(result.value());
  if (!ofd)
    return -EIO;
  retain(session);
  ofd->set_pty_session(session);
  return result.value();
}

int bind_session_stdio(Session *session, int source_fd,
                                   uint16_t bind_mask =
                                       LLVM_LIBC_PTY_ATTACH_ALL) {
  if (!session)
    return -ENOTTY;

  if (bind_mask == 0)
    bind_mask = LLVM_LIBC_PTY_ATTACH_ALL;

  int temp_fd = source_fd;
  bool created_temp = false;
  if (temp_fd < 0) {
    temp_fd = create_slave_fd(session, O_RDWR);
    if (temp_fd < 0)
      return temp_fd;
    created_temp = true;
  }

  auto cleanup_temp = cpp::make_scope_guard([&] {
    if (created_temp && temp_fd >= 0)
      (void)fd_table.release(temp_fd);
  });

  if ((bind_mask & LLVM_LIBC_PTY_ATTACH_STDIN) != 0) {
    auto dup_stdin = fd_table.dup_to(temp_fd, 0);
    if (!dup_stdin.has_value())
      return -dup_stdin.error();
  }
  if ((bind_mask & LLVM_LIBC_PTY_ATTACH_STDOUT) != 0) {
    auto dup_stdout = fd_table.dup_to(temp_fd, 1);
    if (!dup_stdout.has_value())
      return -dup_stdout.error();
  }
  if ((bind_mask & LLVM_LIBC_PTY_ATTACH_STDERR) != 0) {
    auto dup_stderr = fd_table.dup_to(temp_fd, 2);
    if (!dup_stderr.has_value())
      return -dup_stderr.error();
  }

  return 0;
}

int adopt_inherited_current_pty(HANDLE reference,
                                            uint16_t bind_mask) {
  pty_tree::CurrentAttachment attachment = {};
  int attachment_err = pty_tree::duplicate_current_attachment(&attachment);
  if (attachment_err != 0) {
    console_util::close_handle_if_valid(&reference);
    return attachment_err;
  }

  uint32_t id = attachment.pty_id;
  if (!reference) {
    pty_tree::release_current_attachment(&attachment);
    return 0;
  }

  Session *session = allocate_session();
  if (!session) {
    pty_tree::release_current_attachment(&attachment);
    return ENOMEM;
  }
  session->id = id;

  session->shared_state_lock = attachment.state_lock;
  session->shared_state_section = attachment.state_section;
  attachment.state_lock = nullptr;
  attachment.state_section = nullptr;
  pty_tree::release_current_attachment(&attachment);

  if (id != 0 && session->shared_state_section) {
    int view_err =
        pty_tree::map_shared_state_view(session->shared_state_section, id,
                                        &session->shared_state);
    if (view_err != 0) {
      console_util::close_handle_if_valid(&reference);
      destroy_session(session);
      return view_err;
    }
  }

  session->slave_console.reference = reference;
  auto *params = NtCurrentPeb()->ProcessParameters;
  if (params) {
  }
  NTSTATUS status = STATUS_INVALID_HANDLE;
  HANDLE inherited_console = params ? params->ConsoleHandle : nullptr;
  if (params && params->StandardInput && params->StandardOutput) {
    status =
        adopt_inherited_client_handles_from_reference(&session->slave_console);
    if (!NT_SUCCESS(status) && inherited_console) {
      HANDLE local_console = nullptr;
      NTSTATUS dup_status =
          duplicate_noninherited(inherited_console, &local_console);
      if (NT_SUCCESS(dup_status)) {
        status = rebuild_local_client_handles_from_connection(
            &session->slave_console, local_console);
      } else {
        status = dup_status;
      }
    }
  } else {
    HANDLE inherited_connection = inherited_console;
    HANDLE local_connection = nullptr;
    if (inherited_connection) {
      status = duplicate_noninherited(inherited_connection, &local_connection);
      if (NT_SUCCESS(status))
        status = rebuild_local_client_handles_from_connection(
            &session->slave_console, local_connection);
    }
    if (!NT_SUCCESS(status))
      status =
          rebuild_local_client_handles_from_reference(&session->slave_console);
  }
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return windows_util::ntstatus_to_errno(status);
  }
  status = console::publish_handles(session->slave_console.handles,
                                    session->slave_console.reference);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return windows_util::ntstatus_to_errno(status);
  }

  if (id == 0) {
    destroy_session(session);
    return 0;
  }

  int stdio_err = bind_session_stdio(session, -1, bind_mask);
  if (stdio_err < 0) {
    destroy_session(session);
    return -stdio_err;
  }

  set_current_attachment_keepalive(session);
  release(session);
  return 0;
}

ErrorOr<int> create_session(Session **out_session,
                                        const struct termios *termp,
                                        const struct winsize *winp) {
  if (!out_session)
    return Error(EINVAL);
  *out_session = nullptr;

  Session *session = allocate_session();
  if (!session)
    return Error(ENOMEM);

  auto id_result = pty_tree::allocate_pty_id();
  if (!id_result.has_value()) {
    destroy_session(session);
    return Error(id_result.error());
  }
  session->id = id_result.value();

  struct termios initial_attrs = {};
  struct winsize initial_winsize = {};
  termios_defaults::init_default_termios(&initial_attrs);
  init_default_winsize(&initial_winsize);
  maybe_seed_current_winsize(&initial_winsize);
  if (termp)
    initial_attrs = *termp;
  if (winp)
    initial_winsize = *winp;

  auto shared_result = pty_tree::create_shared_state(
      session->id, &session->shared_state_lock, &session->shared_state_section,
      &session->shared_state);
  if (!shared_result.has_value()) {
    destroy_session(session);
    return Error(shared_result.error());
  }
  session->shared_state->attrs = initial_attrs;
  session->shared_state->winsize = initial_winsize;
  session->shared_state->owner_pid =
      static_cast<uint64_t>(static_cast<uintptr_t>(NtCurrentProcessId()));
  session->shared_state->owner_create_time = process_util::current_process_create_time();
  session->shared_state->controller_create_time = 0;
  session->shared_state->controlling_sid = 0;
  session->shared_state->foreground_pgrp = 0;
  session->shared_state->controller_pid = 0;
  session->shared_state->reserved1 = 0;
  session->shared_state->flags = 0;

  NTSTATUS status = STATUS_SUCCESS;

  HANDLE server = nullptr;
  HANDLE reference = nullptr;
  HANDLE child_input = nullptr;
  HANDLE child_output = nullptr;
  HANDLE child_signal = nullptr;
  HANDLE parent_input = nullptr;
  HANDLE parent_output = nullptr;
  HANDLE parent_signal = nullptr;
  HANDLE slave_output_pipe = nullptr;
  HANDLE *inherit_handles =
      static_cast<HANDLE *>(page_alloc(4 * sizeof(HANDLE)));
  WCHAR *image_buffer = static_cast<WCHAR *>(page_alloc(4096));
  WCHAR *command_buffer = static_cast<WCHAR *>(page_alloc(4096));
  if (!inherit_handles || !image_buffer || !command_buffer) {
    if (inherit_handles)
      page_free(inherit_handles);
    if (image_buffer)
      page_free(image_buffer);
    if (command_buffer)
      page_free(command_buffer);
    destroy_session(session);
    return Error(ENOMEM);
  }

  auto cleanup_buffers = cpp::make_scope_guard([&] {
    page_free(command_buffer);
    page_free(image_buffer);
    page_free(inherit_handles);
  });
  auto cleanup_handles = cpp::make_scope_guard([&] {
    console_util::close_handle_if_valid(&child_signal);
    console_util::close_handle_if_valid(&child_output);
    console_util::close_handle_if_valid(&child_input);
    console_util::close_handle_if_valid(&parent_signal);
    console_util::close_handle_if_valid(&parent_output);
    console_util::close_handle_if_valid(&parent_input);
    console_util::close_handle_if_valid(&slave_output_pipe);
    console_util::close_handle_if_valid(&reference);
    console_util::close_handle_if_valid(&server);
  });

  status = condrv::open_condrv_server(&server, GENERIC_ALL, true);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }

  status = condrv::open_condrv_child(&reference, server,
                                     condrv::CONDRV_REFERENCE_NAME);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }

  status = create_pipe_endpair(&parent_input, &child_input, false);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }
  status = create_pipe_endpair(&parent_output, &child_output, true);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }
  status = create_pipe_endpair(&parent_signal, &child_signal, false);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }

  status = duplicate_noninherited(child_output, &slave_output_pipe);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }

  UNICODE_STRING nt_image_path = {};
  UNICODE_STRING command_line = {};
  status = console_util::build_conhost_image_path(image_buffer, 2048, &nt_image_path);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(ENOENT);
  }

  const WCHAR *dos_image_path = nt_image_path.Buffer;
  if ((nt_image_path.Length / sizeof(WCHAR)) <= 4 ||
      dos_image_path[0] != u'\\' || dos_image_path[1] != u'?' ||
      dos_image_path[2] != u'?' || dos_image_path[3] != u'\\') {
    destroy_session(session);
    return Error(ENOENT);
  }
  dos_image_path += 4;

  status = build_headless_command_line(
      command_buffer, 2048, dos_image_path, initial_winsize.ws_col,
      initial_winsize.ws_row, child_signal, server, &command_line);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(ENOMEM);
  }

  inherit_handles[0] = server;
  inherit_handles[1] = child_input;
  inherit_handles[2] = child_output;
  inherit_handles[3] = child_signal;
  HANDLE conhost_process = nullptr;
  int launch_err = launch_headless_conhost(&nt_image_path, &command_line,
                                           inherit_handles, 4, child_input,
                                           child_output, child_output,
                                           &conhost_process);
  if (launch_err != 0) {
    destroy_session(session);
    return Error(launch_err > 0 ? launch_err : EIO);
  }

  console_util::close_handle_if_valid(&child_signal);
  console_util::close_handle_if_valid(&child_output);
  console_util::close_handle_if_valid(&child_input);
  session->conhost_process.reset(conhost_process);

  HANDLE connection = nullptr;
  status = wait_for_condrv_connect(&connection, reference,
                                   session->conhost_process.get());
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(ETIMEDOUT);
  }

  status = condrv::open_condrv_client_handles(&session->slave_console.handles,
                                              connection, false);
  if (!NT_SUCCESS(status)) {
    destroy_session(session);
    return Error(windows_util::ntstatus_to_errno(status));
  }

  session->slave_console.server = server;
  session->slave_console.reference = reference;
  server = nullptr;
  reference = nullptr;

  session->master_input.reset(parent_output);
  session->master_output.reset(parent_input);
  session->slave_output_pipe.reset(slave_output_pipe);
  session->signal_write.reset(parent_signal);
  parent_output = nullptr;
  parent_input = nullptr;
  parent_signal = nullptr;
  slave_output_pipe = nullptr;

  ULONG64 server_pid = 0;
  status = condrv::get_condrv_server_pid(session->slave_console.handles.Connection,
                                         &server_pid);
  if (NT_SUCCESS(status)) {
    HANDLE process = nullptr;
    if (open_process_by_id(server_pid, &process).has_value())
      session->conhost_process.reset(process);
  }

  pty_tree::OwnedHandles join_handles = {};
  join_handles.state_lock = session->shared_state_lock;
  join_handles.state_section = session->shared_state_section;
  join_handles.reference = session->slave_console.reference;
  join_handles.connection = session->slave_console.handles.Connection;
  join_handles.input = session->slave_console.handles.Input;
  join_handles.output = session->slave_console.handles.Output;
  join_handles.error = session->slave_console.handles.Error;
  join_handles.stdout_pipe = session->slave_output_pipe.get();
  auto register_result = pty_tree::register_owned_pty(session->id, join_handles);
  if (!register_result.has_value()) {
    destroy_session(session);
    return Error(register_result.error());
  }
  session->owns_tree_registration = true;

  int err = apply_termios_to_session(session);
  if (err < 0) {
    destroy_session(session);
    return Error(-err);
  }

  cleanup_buffers.dismiss();
  cleanup_handles.dismiss();
  *out_session = session;
  return 0;
}

int validate_master_fd(int fd, OpenFileDescription **ofd_out,
                                   Session **session_out) {
  OpenFileDescription *ofd = get_ofd(fd);
  if (!ofd)
    return -EBADF;
  if (!ofd->is_pty_master())
    return -ENOTTY;
  Session *session = session_from_ofd(ofd);
  if (!session)
    return -ENOTTY;
  if (ofd_out)
    *ofd_out = ofd;
  if (session_out)
    *session_out = session;
  return 0;
}

} // namespace

// --- Public backend API (called by terminal_ops dispatch layer) ---

int validate_pty_fd(int fd, OpenFileDescription **ofd_out,
                    Session **session_out) {
  OpenFileDescription *ofd = get_ofd(fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session)
    return -ENOTTY;
  if (ofd_out)
    *ofd_out = ofd;
  if (session_out)
    *session_out = session;
  return 0;
}

int check_foreground_access(Session *session, TerminalAccessKind kind) {
  pid_t sid = 0, pgid = 0;
  int err = with_shared_state(session, [&](const PtySharedState &shared) {
    sid = shared.controlling_sid;
    pgid = shared.foreground_pgrp;
    return 0;
  });
  if (err < 0)
    return err;
  return enforce_foreground_access(sid != 0, sid, pgid, 0, kind);
}

int pty_get_attr(Session *session, struct termios *t) {
  return with_shared_state(session, [&](const PtySharedState &shared) {
    *t = shared.attrs;
    return 0;
  });
}

int pty_set_attr(Session *session, int actions, const struct termios *t) {
  (void)actions;
  if (!session || !t)
    return -EINVAL;
  return pty_tree::set_attr(session->id, session->shared_state_section, t);
}

int pty_flush(Session *session, int queue_selector) {
  (void)session;
  switch (queue_selector) {
  case TCIFLUSH:
  case TCOFLUSH:
  case TCIOFLUSH:
    return 0;
  default:
    return -EINVAL;
  }
}

int pty_drain(Session *session) {
  (void)session;
  return 0;
}

ErrorOr<pid_t> pty_get_sid(Session *session) {
  pid_t sid = 0;
  int err = with_shared_state(session, [&](const PtySharedState &shared) {
    sid = shared.controlling_sid;
    return 0;
  });
  if (err < 0)
    return Error(-err);
  if (sid == 0)
    return Error(ENOTTY);
  return sid;
}

ErrorOr<pid_t> pty_get_foreground_pgrp(Session *session) {
  pid_t pgid = 0;
  int err = with_shared_state(session, [&](const PtySharedState &shared) {
    pgid = shared.foreground_pgrp;
    return 0;
  });
  if (err < 0)
    return Error(-err);
  if (pgid == 0)
    return Error(ENOTTY);
  return pgid;
}

int pty_set_foreground_pgrp(Session *session, pid_t pgid) {
  if (!session || pgid <= 0)
    return -EINVAL;
  return pty_tree::set_foreground_pgrp(session->id,
                                       session->shared_state_section, pgid);
}

int pty_flow(Session *session, int action) {
  (void)session;
  switch (action) {
  case TCOOFF:
  case TCOON:
  case TCIOFF:
  case TCION:
    return 0;
  default:
    return -EINVAL;
  }
}

int pty_send_break(Session *session) {
  // POSIX break semantics on a pseudo-terminal: no electrical break exists,
  // but if BRKINT is set (and IGNBRK is not), generate SIGINT to the
  // foreground process group and flush queues.
  tcflag_t iflag = 0;
  pid_t fg_pgid = 0;
  int err = with_shared_state(session, [&](const PtySharedState &shared) {
    iflag = shared.attrs.c_iflag;
    fg_pgid = shared.foreground_pgrp;
    return 0;
  });
  if (err < 0)
    return err;

  if ((iflag & IGNBRK) || !(iflag & BRKINT))
    return 0;

  if (fg_pgid > 0)
    signal_state::kill(-static_cast<intptr_t>(fg_pgid), SIGINT);
  else
    signal_state::deliver_process_signal(SIGINT);
  return 0;
}

int pty_get_pending_input_bytes(Session *session, int *count) {
  // PTY input is processed through the ConDrv console_tty line discipline
  // attached to the slave. The PTY session itself does not buffer input;
  // the slave's console_tty state holds the canonical/ready buffers. Since
  // the calling fd is validated as a PTY, report 0 — the kernel pipe has
  // bytes but we cannot query their count without a ConDrv peek ioctl.
  (void)session;
  *count = 0;
  return 0;
}

int pty_get_winsize(Session *session, struct winsize *ws) {
  return with_shared_state(session, [&](const PtySharedState &shared) {
    *ws = shared.winsize;
    return 0;
  });
}

int pty_set_winsize(Session *session, const struct winsize *ws) {
  if (!session || !ws)
    return -EINVAL;

  int err = apply_winsize_to_session(session, *ws);
  if (err < 0)
    return err;

  bool changed = false;
  pid_t foreground_pgrp = 0;
  pid_t controller_pid = 0;
  uint64_t controller_create_time = 0;
  err = with_shared_state_write(session, [&](PtySharedState &shared) {
    const struct winsize &previous = shared.winsize;
    changed = previous.ws_row != ws->ws_row || previous.ws_col != ws->ws_col ||
              previous.ws_xpixel != ws->ws_xpixel ||
              previous.ws_ypixel != ws->ws_ypixel;
    shared.winsize = *ws;
    foreground_pgrp = shared.foreground_pgrp;
    controller_pid = shared.controller_pid;
    controller_create_time = shared.controller_create_time;
    return 0;
  });
  if (err < 0)
    return err;

  if (changed)
    deliver_session_winsize_signal(foreground_pgrp, controller_pid,
                                   controller_create_time);
  return 0;
}

int adopt_inherited_current_pty_startup(HANDLE reference) {
  return adopt_inherited_current_pty(reference,
                                     pty_tree::inherited_attach_flags());
}

bool is_pty(const OpenFileDescription *ofd) {
  return ofd && (ofd->is_pty_master() || ofd->is_pty_slave());
}

bool is_master(const OpenFileDescription *ofd) {
  return ofd && ofd->is_pty_master();
}

bool is_slave(const OpenFileDescription *ofd) {
  return ofd && ofd->is_pty_slave();
}

uintptr_t session_key(const OpenFileDescription *ofd) {
  return reinterpret_cast<uintptr_t>(session_from_ofd(const_cast<OpenFileDescription *>(ofd)));
}

bool is_ptmx_path(const char *path) { return string_util::streq(path, PTMX_PATH); }

bool is_pts_path(const char *path) { return parse_pts_id(path, nullptr); }

ErrorOr<int> duplicate_spawn_handles(const OpenFileDescription *ofd,
                                     SpawnHandles *handles) {
  if (!ofd || !handles)
    return Error(EINVAL);
  if (!ofd->is_pty_slave())
    return Error(ENOTTY);

  Session *session =
      session_from_ofd(const_cast<OpenFileDescription *>(ofd));
  if (!session)
    return Error(ENOTTY);

  NTSTATUS status = duplicate_spawn_session_handles(session, handles);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));
  return 0;
}

void close_spawn_handles(SpawnHandles *handles) {
  close_spawn_session_handles(handles);
}

ErrorOr<int> posix_openpt(int flags) {
  int normalized_flags = normalize_master_open_flags(flags);
  if (normalized_flags < 0)
    return Error(-normalized_flags);

  Session *session = nullptr;
  auto session_result = create_session(&session, nullptr, nullptr);
  if (!session_result.has_value())
    return session_result;

  int master_fd = create_master_fd(session, normalized_flags);
  if (master_fd < 0) {
    release(session);
    return Error(-master_fd);
  }
  if (!get_ofd(master_fd)) {
    release(session);
    return Error(ENXIO);
  }

  release(session);
  return master_fd;
}

int grantpt(int fd) {
  OpenFileDescription *ofd = nullptr;
  Session *session = nullptr;
  int err = validate_master_fd(fd, &ofd, &session);
  if (err < 0)
    return err;
  if (!ofd || !session)
    return -ENOTTY;
  if (session->refcount.load(cpp::MemoryOrder::ACQUIRE) == 0)
    return -EIO;
  if (!session->shared_state || !session->shared_state_lock)
    return -EIO;

  err = lock_shared_state(session);
  if (err < 0)
    return err;
  session->shared_state->flags |= PTY_SHARED_FLAG_GRANTED;
  unlock_shared_state(session);
  return 0;
}

int unlockpt(int fd) {
  Session *session = nullptr;
  int err = validate_master_fd(fd, nullptr, &session);
  if (err < 0)
    return err;
  return with_shared_state_write(session, [&](PtySharedState &shared) {
    if ((shared.flags & PTY_SHARED_FLAG_GRANTED) == 0)
      return -EACCES;
    shared.flags |= PTY_SHARED_FLAG_UNLOCKED;
    return 0;
  });
}

int ptsname_r(int fd, char *buffer, size_t size) {
  Session *session = nullptr;
  int err = validate_master_fd(fd, nullptr, &session);
  if (err < 0)
    return -err;
  return copy_pts_path(buffer, size, session->id);
}

ErrorOr<int> open_pts_path(const char *path, int flags) {
  uint32_t id = 0;
  if (!parse_pts_id(path, &id))
    return Error(ENOENT);

  return open_pts_id(id, flags);
}

ErrorOr<int> open_pts_id(uint32_t id, int flags) {
  if (id == 0)
    return Error(ENOENT);

  int normalized_flags = normalize_slave_open_flags(flags);
  if (normalized_flags < 0)
    return Error(-normalized_flags);

  Session *session = allocate_session();
  if (!session)
    return Error(ENOMEM);
  session->id = id;

  auto local_handles_result = pty_tree::duplicate_local_owned_handles(id);
  if (local_handles_result.has_value()) {
    pty_tree::OwnedHandles handles = local_handles_result.value();
    session->shared_state_lock = handles.state_lock;
    session->shared_state_section = handles.state_section;
    session->slave_console.reference = handles.reference;
    session->slave_console.handles.Connection = handles.connection;
    session->slave_console.handles.Input = handles.input;
    session->slave_console.handles.Output = handles.output;
    session->slave_console.handles.Error = handles.error;
    session->slave_output_pipe.reset(handles.stdout_pipe);
  } else {
    if (local_handles_result.error() != ENOENT) {
      destroy_session(session);
      return Error(local_handles_result.error());
    }
    auto join_result = pty_tree::join_handles(id);
    if (!join_result.has_value()) {
      destroy_session(session);
      return Error(join_result.error());
    }
    pty_tree::OwnedHandles handles = join_result.value();
    session->shared_state_lock = handles.state_lock;
    session->shared_state_section = handles.state_section;
    session->slave_console.reference = handles.reference;
    session->slave_output_pipe.reset(handles.stdout_pipe);
    NTSTATUS rebuild_status =
        rebuild_local_client_handles_from_reference(&session->slave_console);
    console_util::close_handle_if_valid(&handles.connection);
    console_util::close_handle_if_valid(&handles.input);
    console_util::close_handle_if_valid(&handles.output);
    console_util::close_handle_if_valid(&handles.error);
    if (!NT_SUCCESS(rebuild_status)) {
      destroy_session(session);
      return Error(windows_util::ntstatus_to_errno(rebuild_status));
    }
  }

  int view_err = pty_tree::map_shared_state_view(session->shared_state_section,
                                                 id, &session->shared_state);
  if (view_err != 0) {
    destroy_session(session);
    return Error(view_err);
  }

  int slave_fd = create_slave_fd(session, normalized_flags);
  if (slave_fd < 0) {
    destroy_session(session);
    return Error(-slave_fd);
  }
  release(session);
  return slave_fd;
}

void retain(Session *session) {
  if (session)
    session->refcount.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
}

void release(Session *session) {
  if (!session)
    return;
  uint32_t old = session->refcount.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  if (old == 1) {
    (void)console::close(&session->slave_console, false);
    destroy_session(session);
  }
}

void release_opaque(void *session) {
  release(reinterpret_cast<Session *>(session));
}

void clear_current_attachment_keepalive() {
  set_current_attachment_keepalive(nullptr);
}

ErrorOr<int> openpty(int *master_fd, int *slave_fd, char *name,
                     const struct termios *termp,
                     const struct winsize *winp) {
  if (!master_fd || !slave_fd)
    return Error(EINVAL);

  Session *session = nullptr;
  auto session_result = create_session(&session, termp, winp);
  if (!session_result.has_value())
    return session_result;

  int master = create_master_fd(session, O_RDWR);
  if (master < 0) {
    release(session);
    return Error(-master);
  }

  (void)with_shared_state_write(session, [&](PtySharedState &shared) {
    shared.flags |= PTY_SHARED_FLAG_GRANTED | PTY_SHARED_FLAG_UNLOCKED;
    return 0;
  });

  int slave = create_slave_fd(session, O_RDWR);
  if (slave < 0) {
    (void)fd_table.release(master);
    release(session);
    return Error(-slave);
  }

  if (name)
    (void)copy_pts_path(name, pts_path_length(session->id) + 1, session->id);

  release(session);
  *master_fd = master;
  *slave_fd = slave;
  return 0;
}

int login_tty(int fd) {
  Session *session = nullptr;
  int err = validate_pty_fd(fd, nullptr, &session);
  if (err < 0)
    return err;
  if (!session)
    return -ENOTTY;

  pid_t self_pid = static_cast<pid_t>(NtCurrentProcessId());
  if (windows_syscalls::get_session_id() != self_pid) {
    auto sid_result = windows_syscalls::setsid();
    if (!sid_result.has_value())
      return -sid_result.error();
  }

  int shared_err = pty_tree::set_session_controller(
      session->id, session->shared_state_section,
      windows_syscalls::get_session_id(), windows_syscalls::getpgrp(), self_pid,
      process_util::current_process_create_time());
  if (shared_err < 0)
    return shared_err;

  NTSTATUS publish_status = console::publish_handles(
      session->slave_console.handles, session->slave_console.reference);
  if (!NT_SUCCESS(publish_status))
    return -windows_util::ntstatus_to_errno(publish_status);

  pty_tree::CurrentAttachment attachment = {};
  attachment.pty_id = session->id;
  attachment.state_lock = session->shared_state_lock;
  attachment.state_section = session->shared_state_section;
  err = pty_tree::set_current_attached_pty(attachment);
  if (err != 0)
    return err;
  err = bind_session_stdio(session, fd);
  if (err < 0)
    return err;
  set_current_attachment_keepalive(session);

  if (fd != 0 && fd != 1 && fd != 2) {
    auto close_result = fd_table.release(fd);
    if (!close_result.has_value())
      return -close_result.error();
  }

  return 0;
}

int is_terminal_fd(int fd) {
  return validate_pty_fd(fd, nullptr, nullptr);
}

// Initialise a stack-local OFD that proxies a slave PTY fd through to the
// underlying ConDrv console handle.  Used by read() and write() below.
void init_slave_proxy_ofd(OpenFileDescription *proxy,
                                      OpenFileDescription *source,
                                      HANDLE handle) {
  proxy->access_mode = source->access_mode;
  proxy->kind = FileKind::ConDrv;
  proxy->ops = &condrv_ops;
  proxy->immutable_flags = source->immutable_flags;
  proxy->handle = handle;
  proxy->status_flags.store(
      source->status_flags.load(cpp::MemoryOrder::ACQUIRE),
      cpp::MemoryOrder::RELAXED);
}

ssize_t read(OpenFileDescription *ofd, void *buffer, size_t count) {
  Session *session = session_from_ofd(ofd);
  if (!session)
    return -ENOTTY;
  if (ofd->is_pty_slave()) {
    OpenFileDescription proxy = {};
    init_slave_proxy_ofd(&proxy, ofd,
                         session->slave_console.handles.Input);
    ScopedAttachedPtyId attached_pty(session);
    return console_tty::read(&proxy, buffer, count);
  }

  bool nonblocking =
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) != 0;
  return read_pipe_bytes(session->master_input.get(), buffer, count,
                         nonblocking);
}

ssize_t write(OpenFileDescription *ofd, const void *buffer, size_t count) {
  Session *session = session_from_ofd(ofd);
  if (!session)
    return -ENOTTY;
  if (ofd->is_pty_slave()) {
    HANDLE pipe_handle = session->slave_output_pipe.get();
    HANDLE connection_handle = session->slave_console.handles.Connection;
    HANDLE output_handle =
        pipe_handle ? pipe_handle : session->slave_console.handles.Output;

    OpenFileDescription proxy = {};
    init_slave_proxy_ofd(&proxy, ofd, output_handle);
    ScopedAttachedPtyId attached_pty(session);
    ssize_t written = console_tty::write(&proxy, buffer, count);
    if (written != -EBADF || pipe_handle || !connection_handle ||
        session->slave_console.handles.Output == connection_handle)
      return written;

    proxy.handle = connection_handle;
    return console_tty::write(&proxy, buffer, count);
  }

  bool nonblocking =
      (ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE) & O_NONBLOCK) != 0;
  return write_pipe_bytes(session->master_output.get(), buffer, count,
                          nonblocking);
}

void fork_reinit() {
  current_attachment_lock.reset_for_fork();
  // The keepalive session pointer is inherited from the parent and still
  // valid (Session lives in slab memory which survives the address space
  // clone).  Leave the refcount as-is — the child inherits the parent's
  // reference.  Just reset the lock so the child can acquire it.
}

} // namespace vt_pty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

void LIBC_NAMESPACE::internal::vt_pty_fork_reinit() {
  LIBC_NAMESPACE::internal::vt_pty::fork_reinit();
}

int LIBC_NAMESPACE::internal::vt_pty_startup_init() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  HANDLE reference =
      LIBC_NAMESPACE::internal::pty_tree::take_inherited_reference();
  uint32_t id =
      LIBC_NAMESPACE::internal::pty_tree::current_attached_pty_id();
  if (id == 0)
    reference = nullptr;
  if (!reference && id != 0 && params) {
    if (LIBC_NAMESPACE::internal::console_util::is_live_console_handle(
            params->ConsoleHandle))
      reference = params->ConsoleHandle;
  }
  if (!reference)
    return 0;
  (void)LIBC_NAMESPACE::internal::vt_pty::adopt_inherited_current_pty_startup(
      reference);
  return 0;
}
