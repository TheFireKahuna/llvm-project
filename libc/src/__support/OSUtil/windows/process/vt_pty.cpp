//===-- VT-backed PTY session support ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/vt_pty.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/lazy_init_reset.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "include/llvm-libc-macros/termios-macros.h"
#include "include/llvm-libc-types/struct_termios.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/new.h"
#include "src/__support/CPP/scope_guard.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
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
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

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
  // Owned: the kernel mutant guarding the shared-state mapping and the
  // section handle backing that mapping. The actual view pointer
  // (`shared_state`) is unmapped by pty_tree::close_shared_state; after
  // that unmap, these handles are released into close_shared_state for
  // the final NtClose so the destructor does not double-close them.
  windows::ScopedNtHandle shared_state_lock;
  windows::ScopedNtHandle shared_state_section;
  PtySharedState *shared_state;
  uint32_t id;
  bool owns_tree_registration;

  Session()
      : refcount(1), lock(), slave_console(),
        master_input(), master_output(), slave_output_pipe(), signal_write(),
        conhost_process(), shared_state_lock(), shared_state_section(),
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

// NtCreateNamedPipeFile's "default timeout" field is only consulted for
// ops that explicitly request it via a NULL per-op timeout; our pipes use
// synchronous-IO-non-alert so this value is never actually observed. The
// 120s figure matches the Win32 default pipe timeout documented for
// CreateNamedPipe and keeps behavior identical if a future caller stops
// passing an explicit timeout.
constexpr int64_t DEFAULT_PIPE_TIMEOUT_100NS = -1200000000LL;
constexpr ULONG PIPE_BUFFER_BYTES = 64u * 1024u;

// Page-sized scratch buffers used while assembling the conhost image path
// and command line. 4 KiB matches the OS page and is the minimum useful
// alloc; path and command line must share the capacity so indexes stay in
// sync when either one changes.
constexpr size_t IMAGE_SCRATCH_CHARS = 2048;
constexpr size_t COMMAND_SCRATCH_CHARS = 2048;
static_assert(IMAGE_SCRATCH_CHARS * sizeof(WCHAR) <= 4096,
              "image scratch must fit in a single 4KiB page");
static_assert(COMMAND_SCRATCH_CHARS * sizeof(WCHAR) <= 4096,
              "command scratch must fit in a single 4KiB page");

constexpr size_t CONHOST_INHERIT_HANDLE_COUNT = 4;

RawMutex current_attachment_lock;
Session *current_attachment_keepalive = nullptr;

// Typed accessors over PtySharedState::flags. The underlying field stays
// uint16_t (cross-process ABI stored in shared memory, shared with
// pty_tree.cpp) — these just give the call sites readable names and keep
// the raw bit-ops in one place.
// Non-owning view over PtySharedState::flags. The underlying field stays
// uint16_t (cross-process ABI shared with pty_tree.cpp); this type keeps
// the raw bit-ops in one place and gives call sites readable names.
struct PtyFlagView {
  PtySharedState &state;

  [[nodiscard]] bool granted() const {
    return (state.flags & PTY_SHARED_FLAG_GRANTED) != 0;
  }
  void mark_granted() { state.flags |= PTY_SHARED_FLAG_GRANTED; }
  void mark_unlocked() { state.flags |= PTY_SHARED_FLAG_UNLOCKED; }
  void mark_granted_and_unlocked() {
    state.flags |= (PTY_SHARED_FLAG_GRANTED | PTY_SHARED_FLAG_UNLOCKED);
  }
  void mark_hungup() { state.flags |= PTY_SHARED_FLAG_HUNGUP; }
};

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
    current.state_lock = session->shared_state_lock.get();
    current.state_section = session->shared_state_section.get();
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

  // Reserve last byte for NUL terminator.
  cpp::StringStream ss(cpp::span<char>(buffer, size - 1));
  ss << cpp::string_view(PTS_PREFIX, sizeof(PTS_PREFIX) - 1) << id;

  buffer[ss.str().size()] = '\0';
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
  NTSTATUS status = ::NtWaitForSingleObject(session->shared_state_lock.get(),
                                            FALSE, nullptr);
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
    (void)::NtReleaseMutant(session->shared_state_lock.get(), nullptr);
}

// RAII guards replacing the old with_shared_state / with_shared_state_write
// templates. Each call site used to synthesize a unique lambda closure,
// forcing a distinct template instantiation; a plain RAII type compiles to
// a single set of inline functions and frees the call sites to read/write
// PtySharedState fields directly.
//
// Contract: status() returns 0 on successful lock acquisition, or a
// negative errno. Callers must check status() before touching state().
// When status() == 0, destruction releases the mutex (and, for the writer
// variant, publishes the seqlock end bump). On lock-failure the dtor is
// a no-op.
class [[nodiscard]] SharedStateReader {
  Session *session_;
  int status_;

public:
  LIBC_INLINE explicit SharedStateReader(Session *s)
      : session_(s), status_(lock_shared_state(s)) {}
  LIBC_INLINE ~SharedStateReader() {
    if (status_ == 0)
      unlock_shared_state(session_);
  }

  SharedStateReader(const SharedStateReader &) = delete;
  SharedStateReader &operator=(const SharedStateReader &) = delete;

  LIBC_INLINE int status() const { return status_; }
  LIBC_INLINE const PtySharedState &state() const {
    return *session_->shared_state;
  }
};

// Writer variant: wraps the mutation in a seqlock begin/end pair so
// lock-free readers (slave processes with PAGE_READONLY mappings) can
// detect torn writes and retry. The begin bump happens in the ctor
// (paired with a RELEASE store); the end bump is emitted by the dtor
// with a preceding RELEASE fence so all data writes precede the end
// bump in program order.
class [[nodiscard]] SharedStateWriter {
  Session *session_;
  int status_;

  LIBC_INLINE static void bump(uint32_t *seq) {
    __atomic_store_n(
        seq, __atomic_load_n(seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
  }

public:
  LIBC_INLINE explicit SharedStateWriter(Session *s)
      : session_(s), status_(lock_shared_state(s)) {
    if (status_ == 0)
      bump(&session_->shared_state->change_seq);
  }
  LIBC_INLINE ~SharedStateWriter() {
    if (status_ == 0) {
      __atomic_thread_fence(__ATOMIC_RELEASE);
      bump(&session_->shared_state->change_seq);
      unlock_shared_state(session_);
    }
  }

  SharedStateWriter(const SharedStateWriter &) = delete;
  SharedStateWriter &operator=(const SharedStateWriter &) = delete;

  LIBC_INLINE int status() const { return status_; }
  LIBC_INLINE PtySharedState &state() { return *session_->shared_state; }
};

// A ConDrv console handle can be addressed two ways: directly, or via an
// explicit per-session connection handle. The three wrappers below keep
// that dispatch in one place per-op so callers don't bake the conditional
// into every site. Plain inline helpers — no template/lambda indirection.
LIBC_INLINE NTSTATUS session_set_console_mode(Session *session, HANDLE target,
                                              DWORD mode) {
  HANDLE conn = session->slave_console.handles.Connection;
  return conn ? condrv::set_console_mode_on(conn, target, mode)
              : condrv::set_console_mode(target, mode);
}

int apply_termios_to_session(Session *session) {
  if (!session || !session->shared_state ||
      !session->slave_console.handles.Input ||
      !session->slave_console.handles.Output)
    return -EINVAL;

  struct termios attrs = {};
  {
    SharedStateReader reader(session);
    if (reader.status() < 0)
      return reader.status();
    attrs = reader.state().attrs;
  }

  NTSTATUS status = session_set_console_mode(
      session, session->slave_console.handles.Input,
      termios_defaults::desired_input_mode(attrs));
  if (!NT_SUCCESS(status))
    return windows_util::nt_neg_errno(status);

  status = session_set_console_mode(
      session, session->slave_console.handles.Output,
      termios_defaults::desired_output_mode(attrs));
  if (!NT_SUCCESS(status))
    return windows_util::nt_neg_errno(status);
  return 0;
}

LIBC_INLINE NTSTATUS get_session_screen_buffer_info(
    Session *session, condrv::CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!session || !info || !session->slave_console.handles.Output)
    return STATUS_INVALID_HANDLE;

  HANDLE connection = session->slave_console.handles.Connection;
  HANDLE output = session->slave_console.handles.Output;
  return connection
             ? condrv::get_console_screen_buffer_info_ex_on(connection, output,
                                                            info)
             : condrv::get_console_screen_buffer_info_ex(output, info);
}

LIBC_INLINE NTSTATUS set_session_screen_buffer_info(
    Session *session, const condrv::CONSOLE_SCREEN_BUFFER_INFO_EX *info) {
  if (!session || !info || !session->slave_console.handles.Output)
    return STATUS_INVALID_HANDLE;

  HANDLE connection = session->slave_console.handles.Connection;
  HANDLE output = session->slave_console.handles.Output;
  return connection
             ? condrv::set_console_screen_buffer_info_ex_wrapper_on(
                   connection, output, info)
             : condrv::set_console_screen_buffer_info_ex_wrapper(output, info);
}

int apply_winsize_to_session(Session *session,
                                         const struct winsize &ws) {
  condrv::CONSOLE_SCREEN_BUFFER_INFO_EX info = {};
  NTSTATUS status = get_session_screen_buffer_info(session, &info);
  if (!NT_SUCCESS(status))
    return windows_util::nt_neg_errno(status);

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
    return windows_util::nt_neg_errno(status);

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

  windows::ScopedNtHandle process;
  NTSTATUS status = ::NtOpenProcessById(
      process.put(), PROCESS_QUERY_LIMITED_INFORMATION,
      static_cast<DWORD>(controller_pid));
  if (!NT_SUCCESS(status))
    return false;

  uint64_t live_create_time = process_util::query_process_create_time(process.get());
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
using console_util::duplicate_inherited_into;
using windows::WStringStream;
using process_util::process_has_exited;

bool is_connect_retry_status(NTSTATUS status) {
  return status == STATUS_OBJECT_NAME_NOT_FOUND ||
         status == STATUS_OBJECT_PATH_NOT_FOUND;
}

[[maybe_unused]] NTSTATUS
wait_for_condrv_connect(HANDLE *out, HANDLE reference, HANDLE conhost_process,
                        SHORT cols, SHORT rows);

[[maybe_unused]] NTSTATUS rebuild_local_client_handles_from_reference(
    console::Session *session) {
  if (!session || !session->reference)
    return STATUS_INVALID_PARAMETER;

  windows::ScopedNtHandle connection;
  NTSTATUS status = wait_for_condrv_connect(
      connection.put(), session->reference, nullptr,
      static_cast<SHORT>(DEFAULT_COLS), static_cast<SHORT>(DEFAULT_ROWS));
  if (!NT_SUCCESS(status))
    return status;

  condrv::CONDRV_CLIENT_HANDLES rebuilt_handles = {};
  status = condrv::open_condrv_client_handles(&rebuilt_handles, connection.get(),
                                              false);
  if (!NT_SUCCESS(status))
    return status;

  // open_condrv_client_handles stored connection into rebuilt_handles.Connection
  // on success — release ownership so the destructor doesn't close it.
  (void)connection.release();
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

[[maybe_unused]] NTSTATUS
wait_for_condrv_connect(HANDLE *out, HANDLE reference, HANDLE conhost_process,
                        SHORT cols, SHORT rows) {
  if (!out || !reference)
    return STATUS_INVALID_PARAMETER;

  // Attach as a client: NtCreateFile("\\Connect", FILE_CREATE, EA="server"=
  // CONSOLE_SERVER_MSG). This triggers conhost's ConsoleAllocateConsole →
  // SetUpConsole → VtIo::StartIfNeeded, which creates the InputBuffer and
  // starts VtInputThread. A plain FILE_OPEN on \Connect leaves conhost
  // without an attached client, so VtInputThread never starts and
  // ReadConsoleInput IOCTLs never see any records.
  console::SessionOptions opts;
  opts.window_visible = true;
  opts.console_app = true;
  condrv::CONSOLE_SERVER_MSG server_msg = {};
  NTSTATUS status = console::build_server_message(&server_msg, opts);
  if (!NT_SUCCESS(status))
    return status;

  // The PEB-derived defaults put zero into ScreenBufferSize / WindowSize
  // whenever the parent process wasn't itself a console application (typical
  // for our test harnesses and any GUI-subsystem shell). Conhost rejects a
  // zero-sized server message silently and the client attach aborts before
  // VtIo::StartIfNeeded creates pInputBuffer and spins up VtInputThread.
  // Stamp the requested PTY dimensions so the attach always succeeds.
  if (cols <= 0)
    cols = static_cast<SHORT>(DEFAULT_COLS);
  if (rows <= 0)
    rows = static_cast<SHORT>(DEFAULT_ROWS);
  server_msg.ScreenBufferSize.X = cols;
  server_msg.ScreenBufferSize.Y = rows;
  server_msg.WindowSize.X = cols;
  server_msg.WindowSize.Y = rows;

  for (int attempt = 0; attempt != MAX_CONNECT_RETRIES; ++attempt) {
    status =
        condrv::create_condrv_connect(out, reference, server_msg, /*inherit=*/false);
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
  windows::nt_wstring_view npfs_name(npfs_root);

  auto root_oa = windows::named_internal_oa(&npfs_name);
  IO_STATUS_BLOCK iosb = {};

  HANDLE raw_npfs_dir = nullptr;
  NTSTATUS status = ::NtOpenFile(
      &raw_npfs_dir, SYNCHRONIZE | FILE_READ_ATTRIBUTES, &root_oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle npfs_dir(raw_npfs_dir);

  windows::nt_wstring_view empty_name;
  auto pipe_oa = windows::named_oa(&empty_name, npfs_dir.get(), true);

  LARGE_INTEGER timeout = {};
  timeout.QuadPart = DEFAULT_PIPE_TIMEOUT_100NS;

  HANDLE raw_read_end = nullptr;
  iosb = {};
  // ConPTY / kernelbase create the master<->conhost stdio pipes as BYTE
  // type in BYTE read mode. Using MESSAGE_TYPE here framed each parent
  // write as a discrete message; conhost's ReadFile drains one message per
  // call but the VtInputThread state machine processed each message
  // independently rather than as a continuous VT byte stream, which
  // swallowed plain ASCII / win32-input-mode CSI sequences before they
  // reached InteractDispatch::WriteInput. Keep the pipe byte-stream for
  // both directions.
  status = ::NtCreateNamedPipeFile(
      &raw_read_end, FILE_GENERIC_READ | FILE_WRITE_ATTRIBUTES, &pipe_oa,
      &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
      FILE_SYNCHRONOUS_IO_NONALERT, FILE_PIPE_BYTE_STREAM_TYPE,
      FILE_PIPE_BYTE_STREAM_MODE, FILE_PIPE_QUEUE_OPERATION, 1,
      PIPE_BUFFER_BYTES, PIPE_BUFFER_BYTES, &timeout);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle read_end(raw_read_end);

  pipe_oa.RootDirectory = read_end.get();

  HANDLE raw_write_end = nullptr;
  iosb = {};
  status = ::NtOpenFile(&raw_write_end,
                        FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES, &pipe_oa,
                        &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_NON_DIRECTORY_FILE |
                            FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return status;
  windows::ScopedNtHandle write_end(raw_write_end);

  *read_end_out = read_end.release();
  *write_end_out = write_end.release();
  return STATUS_SUCCESS;
}

enum class PipeDirection {
  ParentWrites, // parent holds the write end, child inherits the read end
  ParentReads,  // parent holds the read end, child inherits the write end
};

NTSTATUS create_pipe_endpair(HANDLE *parent_end, HANDLE *child_end,
                             PipeDirection direction) {
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

  const bool parent_reads = direction == PipeDirection::ParentReads;
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

// Internal RAII scratch mirroring SpawnHandles' handle fields. Used only
// inside duplicate_spawn_session_handles so that an early return on any
// single duplicate failure leaves no leaked handles behind. The public
// SpawnHandles struct stays POD because it is copied by value, zero-
// initialised with `= {}`, and crosses the fork/exec boundary via the
// Reserved2Ext ABI in spawn_ops.cpp.
struct SpawnHandlesScratch {
  windows::ScopedNtHandle reference;
  windows::ScopedNtHandle connection;
  windows::ScopedNtHandle input;
  windows::ScopedNtHandle output;
  windows::ScopedNtHandle error;
  windows::ScopedNtHandle stdout_pipe;
  windows::ScopedNtHandle state_lock;
  windows::ScopedNtHandle state_section;

  // Transfer ownership into the caller's POD. After this, the scratch
  // destructor is a no-op.
  void commit(SpawnHandles *out, uintptr_t session_key,
              uint32_t pty_id) noexcept {
    out->reference = reference.release();
    out->connection = connection.release();
    out->input = input.release();
    out->output = output.release();
    out->error = error.release();
    out->stdout_pipe = stdout_pipe.release();
    out->state_lock = state_lock.release();
    out->state_section = state_section.release();
    out->session_key = session_key;
    out->pty_id = pty_id;
  }
};

NTSTATUS duplicate_spawn_session_handles(const Session *session,
                                         SpawnHandles *handles) {
  if (!session || !handles)
    return STATUS_INVALID_PARAMETER;

  *handles = {};

  SpawnHandlesScratch scratch;

  // Eight parallel duplicates, ordered source -> scratch slot. Any failure
  // returns before commit() so the scratch RAII releases what succeeded.
  struct DupEntry {
    HANDLE source;
    windows::ScopedNtHandle *dest;
  };
  const DupEntry entries[] = {
      {session->slave_console.reference,          &scratch.reference},
      {session->shared_state_lock.get(),          &scratch.state_lock},
      {session->shared_state_section.get(),       &scratch.state_section},
      {session->slave_console.handles.Connection, &scratch.connection},
      {session->slave_console.handles.Input,      &scratch.input},
      {session->slave_console.handles.Output,     &scratch.output},
      {session->slave_console.handles.Error,      &scratch.error},
      {session->slave_output_pipe.get(),          &scratch.stdout_pipe},
  };

  for (const DupEntry &e : entries) {
    NTSTATUS status = duplicate_inherited_into(e.source, *e.dest);
    if (!NT_SUCCESS(status))
      return status;
  }

  scratch.commit(handles, reinterpret_cast<uintptr_t>(session), session->id);
  return STATUS_SUCCESS;
}

void close_spawn_session_handles(SpawnHandles *handles) {
  if (!handles)
    return;
  HANDLE *const slots[] = {
      &handles->state_section, &handles->state_lock, &handles->stdout_pipe,
      &handles->error,         &handles->output,     &handles->input,
      &handles->connection,    &handles->reference,
  };
  for (HANDLE *slot : slots)
    console_util::close_handle_if_valid(slot);
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

  WStringStream ss(cpp::span<WCHAR>(buffer, capacity - 1));
  ss << u'"' << image_path << u'"'
     << u" --headless --width " << cols
     << u" --height " << rows
     << u" --signal ";
  ss.write_int<radix::Hex::WithPrefix>(
      reinterpret_cast<uintptr_t>(signal_handle));
  ss << u" --server ";
  ss.write_int<radix::Hex::WithPrefix>(
      reinterpret_cast<uintptr_t>(server_handle));

  if (ss.overflow())
    return STATUS_BUFFER_TOO_SMALL;
  ss.null_terminate();
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
    return windows_util::nt_error(status);
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
    return windows_util::nt_neg_errno(status);
  if (!NT_SUCCESS(iosb.Status))
    return windows_util::nt_neg_errno(iosb.Status);
  return static_cast<int>(iosb.Information);
}

// Shared EAGAIN prologue for nonblocking pipe I/O. On a synchronous-IO
// pipe NtReadFile/NtWriteFile would block in the kernel; we peek via
// FilePipeLocalInformation and fail fast. Returns 0 if the caller should
// proceed, or a negative errno to propagate.
enum class PipePeek { Read, Write };
int peek_pipe_available(HANDLE handle, PipePeek direction) {
  IO_STATUS_BLOCK qiosb = {};
  FILE_PIPE_LOCAL_INFORMATION info = {};
  NTSTATUS qstatus = ::NtQueryInformationFile(
      handle, &qiosb, &info, sizeof(info), FilePipeLocalInformation);
  if (!NT_SUCCESS(qstatus))
    return windows_util::nt_neg_errno(qstatus);
  const ULONG available =
      direction == PipePeek::Read ? info.ReadDataAvailable
                                  : info.WriteQuotaAvailable;
  return available == 0 ? -EAGAIN : 0;
}

ssize_t read_pipe_bytes(HANDLE handle, void *buffer, size_t count,
                                    bool nonblocking) {
  if (!handle)
    return -EBADF;
  if (count == 0)
    return 0;

  if (nonblocking) {
    int peek = peek_pipe_available(handle, PipePeek::Read);
    if (peek < 0)
      return peek;
  }

  IO_STATUS_BLOCK iosb = {};
  ULONG io_len = count > MAX_PIPE_IO ? MAX_PIPE_IO : static_cast<ULONG>(count);
  NTSTATUS status = ::NtReadFile(handle, nullptr, nullptr, nullptr, &iosb,
                                 buffer, io_len, nullptr, nullptr);
  if (!NT_SUCCESS(status)) {
    if (status == STATUS_PIPE_BROKEN || status == STATUS_END_OF_FILE)
      return 0;
    return windows_util::nt_neg_errno(status);
  }
  if (!NT_SUCCESS(iosb.Status)) {
    if (iosb.Status == STATUS_PIPE_BROKEN || iosb.Status == STATUS_END_OF_FILE)
      return 0;
    return windows_util::nt_neg_errno(iosb.Status);
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
    int peek = peek_pipe_available(handle, PipePeek::Write);
    if (peek < 0)
      return peek;
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
        PtyFlagView{*session->shared_state}.mark_hungup();
        unlock_shared_state(session);
      }
      pty_tree::unregister_owned_pty(session->id);
    }
    // Hand ownership of the two handles over to close_shared_state for the
    // final NtClose. release() nulls the ScopedNtHandle fields so the
    // destructor below can't double-close them.
    HANDLE lock_raw = session->shared_state_lock.release();
    HANDLE section_raw = session->shared_state_section.release();
    pty_tree::close_shared_state(&lock_raw, &section_raw,
                                 &session->shared_state);
  }
  session->~Session();
  page_free(session);
}

// Scope-guard RAII: owns a Session* until release() is called. If the
// guard goes out of scope still owning a session, destroy_session runs.
// Collapses the repeated `destroy_session(session); return Error(...);`
// pattern in create_session / adopt_inherited_current_pty / open_pts_id.
struct SessionBuilder {
  Session *session_ = nullptr;

  SessionBuilder() = default;
  explicit SessionBuilder(Session *s) : session_(s) {}
  ~SessionBuilder() {
    if (session_)
      destroy_session(session_);
  }

  SessionBuilder(const SessionBuilder &) = delete;
  SessionBuilder &operator=(const SessionBuilder &) = delete;

  void reset(Session *s) {
    if (session_)
      destroy_session(session_);
    session_ = s;
  }
  Session *get() const { return session_; }
  Session *operator->() const { return session_; }
  explicit operator bool() const { return session_ != nullptr; }

  // Relinquish ownership. The caller now owns the Session.
  [[nodiscard]] Session *release() {
    Session *s = session_;
    session_ = nullptr;
    return s;
  }
};

// Shared fd creation path for both PTY ends. Master ends are backed by
// the ConDrv-side pipe read handle; slave ends are backed by the ConDrv
// input handle. In both cases we hand fd_table a HANDLE and install a
// PTY session pointer on the resulting OFD (with a retain to match the
// eventual OFD destruction).
int create_session_fd(Session *session, FileKind kind, HANDLE handle,
                      int flags) {
  auto result = fd_table.alloc(handle, flags, 0, kind);
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

int create_master_fd(Session *session, int flags) {
  return create_session_fd(session, FileKind::PtyMaster,
                           session->master_input.get(), flags);
}

int create_slave_fd(Session *session, int flags) {
  return create_session_fd(session, FileKind::PtySlave,
                           session->slave_console.handles.Input, flags);
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

  SessionBuilder builder(allocate_session());
  if (!builder) {
    pty_tree::release_current_attachment(&attachment);
    return ENOMEM;
  }
  Session *session = builder.get();
  session->id = id;

  // Transfer ownership of the handles out of the CurrentAttachment POD into
  // the session's RAII fields.
  session->shared_state_lock.reset(attachment.state_lock);
  session->shared_state_section.reset(attachment.state_section);
  attachment.state_lock = nullptr;
  attachment.state_section = nullptr;
  pty_tree::release_current_attachment(&attachment);

  if (id != 0 && session->shared_state_section) {
    int view_err = pty_tree::map_shared_state_view(
        session->shared_state_section.get(), id, &session->shared_state);
    if (view_err != 0) {
      console_util::close_handle_if_valid(&reference);
      return view_err;
    }
  }

  session->slave_console.reference = reference;
  auto *params = NtCurrentPeb()->ProcessParameters;
  NTSTATUS status = STATUS_INVALID_HANDLE;
  HANDLE inherited_console = params ? params->ConsoleHandle : nullptr;
  if (params && params->StandardInput && params->StandardOutput) {
    status =
        rebuild_local_client_handles_from_reference(&session->slave_console);
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
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);
  status = console::publish_handles(session->slave_console.handles,
                                    session->slave_console.reference);
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);

  if (id == 0)
    return 0;

  // bind_session_stdio returns negative errno on failure; this function
  // returns positive errno, so flip the sign explicitly.
  int stdio_err = bind_session_stdio(session, -1, bind_mask);
  if (stdio_err < 0)
    return -stdio_err;

  Session *live = builder.release();
  set_current_attachment_keepalive(live);
  release(live);
  return 0;
}

ErrorOr<int> create_session(Session **out_session,
                                        const struct termios *termp,
                                        const struct winsize *winp) {
  if (!out_session)
    return Error(EINVAL);
  *out_session = nullptr;

  SessionBuilder builder(allocate_session());
  if (!builder)
    return Error(ENOMEM);
  Session *session = builder.get();

  auto id_result = pty_tree::allocate_pty_id();
  if (!id_result.has_value())
    return Error(id_result.error());
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

  // create_shared_state wants raw HANDLE*. Hand it locals, then move the
  // returned handles into the session so ownership is RAII-tracked.
  HANDLE raw_state_lock = nullptr;
  HANDLE raw_state_section = nullptr;
  auto shared_result = pty_tree::create_shared_state(
      session->id, &raw_state_lock, &raw_state_section, &session->shared_state);
  if (!shared_result.has_value())
    return Error(shared_result.error());
  session->shared_state_lock.reset(raw_state_lock);
  session->shared_state_section.reset(raw_state_section);
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
  HANDLE inherit_handles[CONHOST_INHERIT_HANDLE_COUNT] = {};
  ScratchAlloc<WCHAR> image_scratch(IMAGE_SCRATCH_CHARS);
  ScratchAlloc<WCHAR> command_scratch(COMMAND_SCRATCH_CHARS);
  if (!image_scratch || !command_scratch)
    return Error(ENOMEM);
  WCHAR *image_buffer = image_scratch.data();
  WCHAR *command_buffer = command_scratch.data();

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
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);

  status = condrv::open_condrv_child(&reference, server,
                                     condrv::CONDRV_REFERENCE_NAME);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);

  status = create_pipe_endpair(&parent_input, &child_input,
                               PipeDirection::ParentWrites);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);
  status = create_pipe_endpair(&parent_output, &child_output,
                               PipeDirection::ParentReads);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);
  status = create_pipe_endpair(&parent_signal, &child_signal,
                               PipeDirection::ParentWrites);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);

  status = duplicate_noninherited(child_output, &slave_output_pipe);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);

  UNICODE_STRING nt_image_path = {};
  UNICODE_STRING command_line = {};
  status = console_util::build_conhost_image_path(
      image_buffer, IMAGE_SCRATCH_CHARS, &nt_image_path);
  if (!NT_SUCCESS(status))
    return Error(ENOENT);

  const WCHAR *dos_image_path = nt_image_path.Buffer;
  if ((nt_image_path.Length / sizeof(WCHAR)) <= 4 ||
      dos_image_path[0] != u'\\' || dos_image_path[1] != u'?' ||
      dos_image_path[2] != u'?' || dos_image_path[3] != u'\\')
    return Error(ENOENT);
  dos_image_path += 4;

  status = build_headless_command_line(
      command_buffer, COMMAND_SCRATCH_CHARS, dos_image_path,
      initial_winsize.ws_col, initial_winsize.ws_row, child_signal, server,
      &command_line);
  if (!NT_SUCCESS(status))
    return Error(ENOMEM);

  inherit_handles[0] = server;
  inherit_handles[1] = child_input;
  inherit_handles[2] = child_output;
  inherit_handles[3] = child_signal;
  static_assert(CONHOST_INHERIT_HANDLE_COUNT == 4,
                "CONHOST_INHERIT_HANDLE_COUNT must match inherit_handles slots");
  HANDLE conhost_process = nullptr;
  int launch_err = launch_headless_conhost(
      &nt_image_path, &command_line, inherit_handles,
      CONHOST_INHERIT_HANDLE_COUNT, child_input, child_output, child_output,
      &conhost_process);
  if (launch_err != 0)
    return Error(launch_err > 0 ? launch_err : EIO);

  console_util::close_handle_if_valid(&child_signal);
  console_util::close_handle_if_valid(&child_output);
  console_util::close_handle_if_valid(&child_input);
  session->conhost_process.reset(conhost_process);

  HANDLE connection = nullptr;
  status = wait_for_condrv_connect(&connection, reference,
                                   session->conhost_process.get(),
                                   static_cast<SHORT>(initial_winsize.ws_col),
                                   static_cast<SHORT>(initial_winsize.ws_row));
  if (!NT_SUCCESS(status))
    return Error(ETIMEDOUT);

  // Match kernelbase's client attach sequence (Function_1800F90C4):
  // \Input opened FILE_CREATE relative to \Reference (not \Connect), and
  // \Output opened FILE_CREATE relative to \Input. Routing \Input through
  // \Connect still resolves in the driver for inject-side IOCTLs, but the
  // resulting handle-table entry isn't the one conhost's InteractDispatch ->
  // GetActiveInputBuffer() picks up for VtInputThread-produced records.
  status = condrv::create_condrv_client_handles(&session->slave_console.handles,
                                                connection, false, reference);
  if (!NT_SUCCESS(status))
    return windows_util::nt_error(status);

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
  join_handles.state_lock = session->shared_state_lock.get();
  join_handles.state_section = session->shared_state_section.get();
  join_handles.reference = session->slave_console.reference;
  join_handles.connection = session->slave_console.handles.Connection;
  join_handles.input = session->slave_console.handles.Input;
  join_handles.output = session->slave_console.handles.Output;
  join_handles.error = session->slave_console.handles.Error;
  join_handles.stdout_pipe = session->slave_output_pipe.get();
  auto register_result = pty_tree::register_owned_pty(session->id, join_handles);
  if (!register_result.has_value())
    return Error(register_result.error());
  session->owns_tree_registration = true;

  int err = apply_termios_to_session(session);
  if (err < 0)
    return Error(-err);

  cleanup_handles.dismiss();
  *out_session = builder.release();
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
  pid_t sid = 0;
  pid_t pgid = 0;
  {
    SharedStateReader reader(session);
    if (reader.status() < 0)
      return reader.status();
    sid = reader.state().controlling_sid;
    pgid = reader.state().foreground_pgrp;
  }
  return enforce_foreground_access(sid != 0, sid, pgid, 0, kind);
}

int pty_get_attr(Session *session, struct termios *t) {
  SharedStateReader reader(session);
  if (reader.status() < 0)
    return reader.status();
  *t = reader.state().attrs;
  return 0;
}

int pty_set_attr(Session *session, int actions, const struct termios *t) {
  (void)actions;
  if (!session || !t)
    return -EINVAL;
  return pty_tree::set_attr(session->id, session->shared_state_section.get(),
                            t);
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
  {
    SharedStateReader reader(session);
    if (reader.status() < 0)
      return Error(-reader.status());
    sid = reader.state().controlling_sid;
  }
  if (sid == 0)
    return Error(ENOTTY);
  return sid;
}

ErrorOr<pid_t> pty_get_foreground_pgrp(Session *session) {
  pid_t pgid = 0;
  {
    SharedStateReader reader(session);
    if (reader.status() < 0)
      return Error(-reader.status());
    pgid = reader.state().foreground_pgrp;
  }
  if (pgid == 0)
    return Error(ENOTTY);
  return pgid;
}

int pty_set_foreground_pgrp(Session *session, pid_t pgid) {
  if (!session || pgid <= 0)
    return -EINVAL;
  return pty_tree::set_foreground_pgrp(
      session->id, session->shared_state_section.get(), pgid);
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
  {
    SharedStateReader reader(session);
    if (reader.status() < 0)
      return reader.status();
    iflag = reader.state().attrs.c_iflag;
    fg_pgid = reader.state().foreground_pgrp;
  }

  if ((iflag & IGNBRK) || !(iflag & BRKINT))
    return 0;

  if (fg_pgid > 0)
    (void)signal_state::kill(-static_cast<intptr_t>(fg_pgid), SIGINT);
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
  SharedStateReader reader(session);
  if (reader.status() < 0)
    return reader.status();
  *ws = reader.state().winsize;
  return 0;
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
  {
    SharedStateWriter writer(session);
    if (writer.status() < 0)
      return writer.status();
    PtySharedState &shared = writer.state();
    const struct winsize &previous = shared.winsize;
    changed = previous.ws_row != ws->ws_row || previous.ws_col != ws->ws_col ||
              previous.ws_xpixel != ws->ws_xpixel ||
              previous.ws_ypixel != ws->ws_ypixel;
    shared.winsize = *ws;
    foreground_pgrp = shared.foreground_pgrp;
    controller_pid = shared.controller_pid;
    controller_create_time = shared.controller_create_time;
  }

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
    return windows_util::nt_error(status);
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
  PtyFlagView{*session->shared_state}.mark_granted();
  unlock_shared_state(session);
  return 0;
}

int unlockpt(int fd) {
  Session *session = nullptr;
  int err = validate_master_fd(fd, nullptr, &session);
  if (err < 0)
    return err;

  SharedStateWriter writer(session);
  if (writer.status() < 0)
    return writer.status();
  PtyFlagView flags{writer.state()};
  if (!flags.granted())
    return -EACCES;
  flags.mark_unlocked();
  return 0;
}

int ptsname_r(int fd, char *buffer, size_t size) {
  // POSIX ptsname_r returns positive errno. validate_master_fd uses the
  // internal negative-errno convention, copy_pts_path uses positive.
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

  SessionBuilder builder(allocate_session());
  if (!builder)
    return Error(ENOMEM);
  Session *session = builder.get();
  session->id = id;

  auto local_handles_result = pty_tree::duplicate_local_owned_handles(id);
  if (local_handles_result.has_value()) {
    pty_tree::OwnedHandles handles = local_handles_result.value();
    session->shared_state_lock.reset(handles.state_lock);
    session->shared_state_section.reset(handles.state_section);
    session->slave_console.reference = handles.reference;
    session->slave_console.handles.Connection = handles.connection;
    session->slave_console.handles.Input = handles.input;
    session->slave_console.handles.Output = handles.output;
    session->slave_console.handles.Error = handles.error;
    session->slave_output_pipe.reset(handles.stdout_pipe);
  } else {
    if (local_handles_result.error() != ENOENT)
      return Error(local_handles_result.error());
    auto join_result = pty_tree::join_handles(id);
    if (!join_result.has_value())
      return Error(join_result.error());
    pty_tree::OwnedHandles handles = join_result.value();
    session->shared_state_lock.reset(handles.state_lock);
    session->shared_state_section.reset(handles.state_section);
    session->slave_console.reference = handles.reference;
    session->slave_output_pipe.reset(handles.stdout_pipe);
    NTSTATUS rebuild_status =
        rebuild_local_client_handles_from_reference(&session->slave_console);
    console_util::close_handle_if_valid(&handles.connection);
    console_util::close_handle_if_valid(&handles.input);
    console_util::close_handle_if_valid(&handles.output);
    console_util::close_handle_if_valid(&handles.error);
    if (!NT_SUCCESS(rebuild_status))
      return windows_util::nt_error(rebuild_status);
  }

  int view_err = pty_tree::map_shared_state_view(
      session->shared_state_section.get(), id, &session->shared_state);
  if (view_err != 0)
    return Error(view_err);

  int slave_fd = create_slave_fd(session, normalized_flags);
  if (slave_fd < 0)
    return Error(-slave_fd);
  release(builder.release());
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

  {
    SharedStateWriter writer(session);
    if (writer.status() == 0)
      PtyFlagView{writer.state()}.mark_granted_and_unlocked();
  }

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
      session->id, session->shared_state_section.get(),
      windows_syscalls::get_session_id(), windows_syscalls::getpgrp(), self_pid,
      process_util::current_process_create_time());
  if (shared_err < 0)
    return shared_err;

  NTSTATUS publish_status = console::publish_handles(
      session->slave_console.handles, session->slave_console.reference);
  if (!NT_SUCCESS(publish_status))
    return windows_util::nt_neg_errno(publish_status);

  pty_tree::CurrentAttachment attachment = {};
  attachment.pty_id = session->id;
  attachment.state_lock = session->shared_state_lock.get();
  attachment.state_section = session->shared_state_section.get();
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
    // Route the ConDrv IOCTLs through this session's Connection handle
    // rather than PEB ConsoleHandle — the test / child process's PEB
    // console is unrelated to this PTY's server, so the default PEB
    // lookup would either miss or target the wrong conhost.
    return console_tty::read(&proxy, buffer, count,
                             session->slave_console.handles.Connection);
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

// ---------------------------------------------------------------------------
// Debug entry points exported from c.dll for use by pty_probe. These wrap the
// slave-side Input handle with the session Connection as the IOCTL target so
// that pending counts and record injection can be measured side-by-side.
// Diagnoses whether conhost's InteractDispatch::WriteInput lands in the same
// InputBuffer our Input handle resolves to.
//
// Export surface: listed in c.def (pty_debug_exports block of
// generate_libc_entrypoints_def). Source-level dllexport is intentionally
// absent so vt_pty.cpp.obj carries no -export directive — this keeps the
// OBJ safe to pull into a unit test that also links c.lib.
// ---------------------------------------------------------------------------
extern "C" {

int
__llvm_libc_pty_debug_pending(int slave_fd) {
  OpenFileDescription *ofd = get_ofd(slave_fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session || !ofd->is_pty_slave())
    return -ENOTTY;
  HANDLE input = session->slave_console.handles.Input;
  HANDLE connection = session->slave_console.handles.Connection;
  if (!input || !connection)
    return -ENOTTY;
  condrv::CONSOLE_GETNUMBEROFINPUTEVENTS_MSG payload = {};
  NTSTATUS st = condrv::issue_console_message_on(
      connection, input, condrv::OP_GET_NUMBER_OF_INPUT_EVENTS, payload);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);
  return static_cast<int>(payload.ReadyEvents);
}

int
__llvm_libc_pty_debug_input_mode(int slave_fd) {
  OpenFileDescription *ofd = get_ofd(slave_fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session || !ofd->is_pty_slave())
    return -ENOTTY;
  HANDLE input = session->slave_console.handles.Input;
  HANDLE connection = session->slave_console.handles.Connection;
  if (!input || !connection)
    return -ENOTTY;
  DWORD mode = 0;
  NTSTATUS st = condrv::get_console_mode_on(connection, input, &mode);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);
  return static_cast<int>(mode);
}

// Reads up to 8 input records from the slave's Input via Connection IOCTL.
// out[0] = record count; out[1..] = packed (EventType, VK, UnicodeChar, CKS)
// tuples. Lets the probe verify what's actually queued in gci.pInputBuffer.
int
__llvm_libc_pty_debug_peek_input(int slave_fd, int *out, int out_max,
                                 int unicode) {
  OpenFileDescription *ofd = get_ofd(slave_fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session || !ofd->is_pty_slave())
    return -ENOTTY;
  HANDLE input = session->slave_console.handles.Input;
  HANDLE connection = session->slave_console.handles.Connection;
  if (!input || !connection || !out || out_max < 1)
    return -EINVAL;

  condrv::CONSOLE_INPUT_RECORD recs[8] = {};
  DWORD count = 0;
  NTSTATUS st = condrv::read_console_input_ex(
      input, recs, 8, &count, condrv::CONSOLE_READ_NOWAIT, unicode != 0,
      connection);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);

  out[0] = static_cast<int>(count);
  int slot = 1;
  for (DWORD i = 0; i < count && slot + 3 < out_max; ++i) {
    out[slot + 0] = recs[i].EventType;
    if (recs[i].EventType == condrv::KEY_EVENT) {
      out[slot + 1] = recs[i].Event.KeyEvent.VirtualKeyCode;
      out[slot + 2] = recs[i].Event.KeyEvent.Character.UnicodeChar;
      out[slot + 3] = static_cast<int>(recs[i].Event.KeyEvent.ControlKeyState);
    } else {
      out[slot + 1] = 0;
      out[slot + 2] = 0;
      out[slot + 3] = 0;
    }
    slot += 4;
  }
  return 0;
}

// Mirrors try_read_input_record() exactly (max_records=1, CONSOLE_READ_NOWAIT,
// unicode=true, io_target=Connection). Returns:
//   >= 0  — (status << 8) | record_count, so caller can see both.
//   < 0   — -errno on failure before the IOCTL.
// Does NOT go through process_input_record / state machine — just the raw read.
int
__llvm_libc_pty_debug_single_read(int slave_fd) {
  OpenFileDescription *ofd = get_ofd(slave_fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session || !ofd->is_pty_slave())
    return -ENOTTY;
  HANDLE input = session->slave_console.handles.Input;
  HANDLE connection = session->slave_console.handles.Connection;
  if (!input || !connection)
    return -ENOTTY;
  condrv::CONSOLE_INPUT_RECORD rec = {};
  DWORD count = 0;
  NTSTATUS st = condrv::read_console_input_ex(
      input, &rec, 1, &count, condrv::CONSOLE_READ_NOWAIT,
      /*unicode=*/true, connection);
  return (static_cast<int>(st) << 8) | static_cast<int>(count & 0xFF);
}

int
__llvm_libc_pty_debug_inject_key(int slave_fd, int ch) {
  OpenFileDescription *ofd = get_ofd(slave_fd);
  if (!ofd)
    return -EBADF;
  Session *session = session_from_ofd(ofd);
  if (!session || !ofd->is_pty_slave())
    return -ENOTTY;
  HANDLE input = session->slave_console.handles.Input;
  HANDLE connection = session->slave_console.handles.Connection;
  if (!input || !connection)
    return -ENOTTY;

  condrv::CONSOLE_INPUT_RECORD rec = {};
  rec.EventType = condrv::KEY_EVENT;
  rec.Event.KeyEvent.KeyDown = TRUE;
  rec.Event.KeyEvent.RepeatCount = 1;
  rec.Event.KeyEvent.VirtualKeyCode = static_cast<USHORT>(ch & 0xFF);
  rec.Event.KeyEvent.VirtualScanCode = 0;
  rec.Event.KeyEvent.Character.UnicodeChar =
      static_cast<WCHAR>(static_cast<unsigned char>(ch));
  rec.Event.KeyEvent.ControlKeyState = 0;

  condrv::CONSOLE_WRITECONSOLEINPUT_REQUEST req = {};
  req.ObjectHandle = input;
  req.Records = &rec;
  req.RecordCount = 1;
  req.Unicode = TRUE;
  req.Append = TRUE;
  condrv::CONSOLE_WRITECONSOLEINPUT_RESULT res = {};
  NTSTATUS st = condrv::write_console_input_on(connection, req, &res);
  if (!NT_SUCCESS(st))
    return -windows_util::ntstatus_to_errno(st);
  return 0;
}

} // extern "C"

} // namespace vt_pty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

void LIBC_NAMESPACE::internal::vt_pty_fork_reinit() {
  LIBC_NAMESPACE::internal::vt_pty::fork_reinit();
}

// First-use gate for inherited attached-PTY adoption. The InitFn
// consumes the PEB-inherited reference and (if there's a recorded
// attached pty id) adopts it as the process's current vt_pty session.
//
// Callers through pty_tree::current_attached_pty_id / has_current_attached_pty
// observe adoption before they can ask "is there a current attached PTY?"
// After exec_self_hollow() the gate is cleared via `.libclzr`, so the
// new image re-runs adoption against its own PEB on the first query.
//
// Kept at `LIBC_NAMESPACE::internal` scope (not in the file's anonymous
// namespace) so LIBC_REGISTER_LAZY_RESET's thunk can resolve the symbol
// via its fully-qualified name.
namespace LIBC_NAMESPACE_DECL {
namespace internal {
static int vt_pty_init_impl() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  HANDLE reference =
      LIBC_NAMESPACE::internal::pty_tree::take_inherited_reference();
  // Use the _unchecked variant to avoid re-entering this gate.
  uint32_t id = LIBC_NAMESPACE::internal::pty_tree::
      current_attached_pty_id_unchecked();
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
LazyInit<&vt_pty_init_impl> g_vt_pty_init;
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace vt_pty {
void ensure_adoption() {
  ::LIBC_NAMESPACE::internal::g_vt_pty_init.ensure();
}
} // namespace vt_pty
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register into `.libclzr$M` so exec_self_hollow() clears the gate
// during image swap. The new image has its own PEB Reserved2 data that
// must be re-consumed; a stale kReady would leak the inherited reference
// and skip adoption for the post-exec image.
LIBC_REGISTER_LAZY_RESET(vt_pty,
                         ::LIBC_NAMESPACE::internal::g_vt_pty_init)

LIBC_REGISTER_FORK_REINIT(vt_pty,
                          ::LIBC_NAMESPACE::internal::kForkPrioVtPty,
                          &::LIBC_NAMESPACE::internal::vt_pty_fork_reinit)
