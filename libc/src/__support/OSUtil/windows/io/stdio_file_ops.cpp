//===-- Windows internal stdio file lifecycle operations -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal kernel functions for stdio file lifecycle on Windows (fclose,
// fdopen, freopen, tmpfile). These implement Linux syscall semantics in
// userspace: 0 on success, -errno on failure. FILE*-producing functions
// use an output parameter. Called from windows_syscalls:: wrappers.
//
//===----------------------------------------------------------------------===//

#include "stdio_file_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/stdio_macros.h"
#include "hdr/unistd_macros.h"
#include "hdr/types/FILE.h"
#include "hdr/types/pid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/span.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/CPP/new.h"
#include "src/__support/File/file.h"
#include "src/__support/File/windows/file.h"
#include "src/__support/OSUtil/windows/alloc/legacy/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/fd/file_ops_table.h"
#include "src/__support/OSUtil/windows/fd/file_pool.h"
#include "src/__support/OSUtil/windows/io/file_ops.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/error_or.h"
#include "src/__support/OSUtil/windows/process/spawn_ops.h"
#include "src/__support/OSUtil/windows/process/wait_ops.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/OSUtil/windows/nt/nt_wstringstream.h"
#include "src/__support/OSUtil/windows/temp_path.h"
#include "src/__support/alloc-checker.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/spawn/file_actions.h"
#include "src/stdio/stderr.h"
#include "src/stdio/stdin.h"
#include "src/stdio/stdout.h"
#include "src/unistd/environ.h"
#include "src/unistd/getentropy.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"

#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ---- fclose_impl -----------------------------------------------------------

intptr_t fclose_impl(::FILE *stream) {
  auto *file = reinterpret_cast<File *>(stream);

  // Capture identity before close frees the object.
  bool is_stdin = (file == reinterpret_cast<File *>(LIBC_NAMESPACE::stdin));
  bool is_stdout = (file == reinterpret_cast<File *>(LIBC_NAMESPACE::stdout));
  bool is_stderr = (file == reinterpret_cast<File *>(LIBC_NAMESPACE::stderr));

  // File::close() flushes, frees the owned buffer, and calls platform_close
  // which handles all NT resource cleanup, fd_table release, and pool return.
  int result = file->close();

  // Null out global std stream pointers so they aren't dangling.
  if (is_stdin)
    LIBC_NAMESPACE::stdin = nullptr;
  else if (is_stdout)
    LIBC_NAMESPACE::stdout = nullptr;
  else if (is_stderr)
    LIBC_NAMESPACE::stderr = nullptr;

  if (result != 0)
    return -result;
  return 0;
}

// ---- fdopen_impl -----------------------------------------------------------

intptr_t fdopen_impl(int fd, const char *mode, ::FILE **out) {
  auto *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;

  // If a File is already constructed for this fd, return it.
  if (ofd->file_ptr) {
    *out = reinterpret_cast<::FILE *>(ofd->file_ptr);
    return 0;
  }

  using ModeFlags = File::ModeFlags;
  ModeFlags modeflags = File::mode_flags(mode);
  if (modeflags == 0)
    return -EINVAL;

  HANDLE handle = ofd->handle;
  // Allocate a pool slot for the File object.
  void *slot = internal::file_pool::alloc();
  if (!slot)
    return -ENOMEM;

  bool use_ring = ofd->is_seekable() && !is_ioring_emulated();

  if (!use_ring) {
    // Non-ring fds (console, pipe, sync fallback) use NtReadFile/NtWriteFile.
    AllocChecker ac;
    auto *buf = new (ac) uint8_t[File::DEFAULT_BUFFER_SIZE];
    if (!ac) {
      internal::file_pool::free(slot);
      return -ENOMEM;
    }

    // Pipe handles are opened in overlapped mode (for IoRing in raw read()).
    // WindowsFile needs a completion event for NtReadFile/NtWriteFile on
    // overlapped handles — without one, NtReadFile returns STATUS_PENDING
    // and there is nothing to wait on, causing false-EOF returns.
    HANDLE completion_event = nullptr;
    if (ofd->is_pipe()) {
      auto oa = windows::internal_oa();
      NTSTATUS evst =
          ::NtCreateEvent(&completion_event, EVENT_ALL_ACCESS, &oa,
                          SynchronizationEvent, FALSE);
      if (!NT_SUCCESS(evst)) {
        delete[] buf;
        internal::file_pool::free(slot);
        return -ENOMEM;
      }
    }

    auto *file = new (slot) WindowsFile(
        handle, buf, File::DEFAULT_BUFFER_SIZE, _IOFBF, true, modeflags, fd,
        completion_event);
    ofd->file_ptr = file;
    File::add_file(file);
    *out = reinterpret_cast<::FILE *>(file);
    return 0;
  }

  // Disk fds get an IoRingFile (ring is per-thread, created lazily).
  AllocChecker ac;
  auto *buf = new (ac) uint8_t[IoRingFile::IORING_BUFFER_SIZE];
  if (!ac) {
    internal::file_pool::free(slot);
    return -ENOMEM;
  }

  auto *file = new (slot) IoRingFile(
      handle, buf, IoRingFile::IORING_BUFFER_SIZE,
      _IOFBF, true, modeflags, fd);
  ofd->file_ptr = file;
  File::add_file(file);
  *out = reinterpret_cast<::FILE *>(file);
  return 0;
}

// ---- freopen_impl ----------------------------------------------------------

// Query the full NT object name from a handle into path_buf.
// Returns the length in WCHARs, or 0 on failure (returns -errno via errout).
static size_t query_handle_nt_path(HANDLE h, WCHAR *path_buf,
                                   size_t max_wchars, long *errout) {
  // NtQueryObject(ObjectNameInformation) returns OBJECT_NAME_INFORMATION
  // (a UNICODE_STRING) followed by the string data in contiguous memory.
  // Use the caller's path_buf as the output buffer.
  constexpr size_t HEADER_SIZE = sizeof(OBJECT_NAME_INFORMATION);
  ULONG buf_bytes =
      static_cast<ULONG>(HEADER_SIZE + max_wchars * sizeof(WCHAR));
  // Overlay the header at the start of path_buf. The string data written
  // by the kernel lands right after the header, still within path_buf.
  auto *oni = reinterpret_cast<OBJECT_NAME_INFORMATION *>(path_buf);
  ULONG ret_len;
  NTSTATUS status = NtQueryObject(h, ObjectNameInformation, oni, buf_bytes,
                                  &ret_len);
  if (!NT_SUCCESS(status)) {
    *errout = -EIO;
    return 0;
  }

  size_t name_len = oni->Name.Length / sizeof(WCHAR);
  if (name_len == 0) {
    *errout = -ENOENT;
    return 0;
  }

  // The string sits after the UNICODE_STRING header. Move it to the
  // start of path_buf so the caller can use it directly.
  WCHAR *src = oni->Name.Buffer;
  __builtin_memcpy(path_buf, src, name_len * sizeof(WCHAR));
  path_buf[name_len] = u'\0';
  return name_len;
}

intptr_t freopen_impl(const char *path, const char *mode, ::FILE *stream,
                  ::FILE **out) {
  if (!mode || !stream)
    return -EINVAL;

  auto *old_file = reinterpret_cast<File *>(stream);
  int fd = get_fileno(old_file);
  if (fd < 0)
    return -EBADF;
  auto *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;

  // Parse mode string early — needed for both code paths.
  using ModeFlags = File::ModeFlags;
  auto modeflags = File::mode_flags(mode);
  if (modeflags == 0)
    return -EINVAL;

  // For NULL path, query the file's NT path before closing the handle.
  auto path_s = path_scratch();
  if (!path_s) return -ENOMEM;
  WCHAR *path_buf = path_s.data();
  size_t path_len = 0;

  if (!path) {
    // Non-disk handles can't be reopened by path — just update mode flags.
    if (!ofd->is_seekable()) {
      reinterpret_cast<WindowsFileBase *>(old_file)->set_mode(modeflags);
      old_file->reset_orientation();
      *out = stream;
      return 0;
    }

    HANDLE h = ofd->handle;
    if (!h)
      return -EBADF;

    long query_err = 0;
    path_len = query_handle_nt_path(h, path_buf, path_s.size(),
                                    &query_err);
    if (path_len == 0)
      return query_err; // already -errno
  }

  // Flush buffered data, then close NT resources WITHOUT freeing the pool
  // slot or releasing the fd — freopen reconstructs the File in-place at the
  // same address so the FILE* pointer remains valid (freopen contract).
  //
  // We cannot use File::close() here because platform_close is self-contained
  // (it frees the pool slot and releases the fd). Instead, flush and call
  // close_nt_resources which only tears down NT handles/pipelines/buffers.
  old_file->flush();

  // Remove from global file list BEFORE in-place reconstruction, because the
  // File constructor reinitializes prev/next to nullptr (would corrupt list).
  // We re-add after constructing the new File object below.
  File::remove_file(old_file);

  bool old_is_ring = ofd->is_seekable() && !is_ioring_emulated();
  if (old_is_ring)
    static_cast<IoRingFile *>(ofd->file_ptr)->close_nt_resources();
  else
    static_cast<WindowsFile *>(ofd->file_ptr)->close_nt_resources();

  // Handle is closed; clear stale pointer so OFD doesn't double-close.
  ofd->handle = nullptr;

  // Translate to NT access and disposition (mirrors openfile).
  ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  ULONG disposition;

  if (modeflags & ModeFlags(File::OpenMode::APPEND)) {
    access |= FILE_APPEND_DATA | FILE_READ_DATA;
    disposition = FILE_OPEN_IF;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_WRITE_DATA;
  } else if (modeflags & ModeFlags(File::OpenMode::WRITE)) {
    access |= FILE_WRITE_DATA;
    disposition = FILE_OVERWRITE_IF;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_READ_DATA;
  } else {
    access |= FILE_READ_DATA;
    disposition = FILE_OPEN;
    if (modeflags & ModeFlags(File::OpenMode::PLUS))
      access |= FILE_WRITE_DATA;
  }

  if (modeflags & ModeFlags(File::CreateType::EXCLUSIVE))
    disposition = FILE_CREATE;

  // Convert user-supplied path, or use the pre-queried NT path.
  if (path) {
    using LIBC_NAMESPACE::cpp::string_view;
    string_view sv(path);
    auto nt = to_nt_path(sv, path_buf, path_s.size());
    if (!nt.has_value())
      return -nt.error();
    path_len = nt.value();
  }

  OBJECT_ATTRIBUTES oa;
  windows::nt_wstring_view name(path_buf, path_len);
  init_object_attributes(&oa, &name);

  HANDLE new_handle;
  ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  bool use_ring = !is_ioring_emulated();

  NTSTATUS status;
  if (use_ring) {
    status = open_overlapped(&oa, access, disposition, share, &new_handle);
  } else {
    status = open_sync_alertable(&oa, access, disposition, share, &new_handle);
  }

  if (!NT_SUCCESS(status)) {
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND)
      return -ENOENT;
    else if (status == STATUS_ACCESS_DENIED)
      return -EACCES;
    else
      return -EIO;
  }

  // Determine file kind for the new handle.
  FileKind new_kind = internal::FdTable::classify_handle(
      new_handle, internal::FdTable::query_file_type(new_handle));

  bool append = modeflags & ModeFlags(File::OpenMode::APPEND);

  // Release old kind-specific resources before changing identity.
  // If release_aux returns true, the old handle should be closed by us
  // (reinit_for_reopen will overwrite ofd->handle with the new one).
  HANDLE old_handle = ofd->handle;
  bool close_old = true;
  if (ofd->ops && ofd->ops->release_aux)
    close_old = ofd->ops->release_aux(ofd);
  if (close_old && old_handle)
    NtClose(old_handle);

  // Build open_flags from the mode string for reinit_for_reopen().
  int accmode = 0;
  if (modeflags & ModeFlags(File::OpenMode::READ))
    accmode = O_RDONLY;
  if (modeflags & ModeFlags(File::OpenMode::WRITE))
    accmode = O_WRONLY;
  if ((modeflags & ModeFlags(File::OpenMode::READ)) &&
      (modeflags & ModeFlags(File::OpenMode::WRITE)))
    accmode = O_RDWR;
  int open_flags = accmode;
  if (append)
    open_flags |= O_APPEND;

  // Reinitialize the OFD with the new handle/kind, preserving refcount
  // and file_ptr (the OFD is still live — freopen doesn't change those).
  ofd->reinit_for_reopen(new_handle, open_flags, new_kind,
                         kind_to_ops(new_kind));

  if (!use_ring) {
    // Non-ring file (console) — create a sync WindowsFile.
    AllocChecker ac;
    auto *buf = new (ac) uint8_t[File::DEFAULT_BUFFER_SIZE];
    if (!ac) {
      NtClose(new_handle);
      ofd->handle = nullptr;
      return -ENOMEM;
    }
    new (ofd->file_ptr) WindowsFile(
        new_handle, buf, File::DEFAULT_BUFFER_SIZE, _IOFBF, true, modeflags,
        fd, nullptr);
    File::add_file(ofd->file_ptr);
    *out = stream;
    return 0;
  }

  // Disk file — create an IoRingFile.
  AllocChecker ac;
  auto *buf = new (ac) uint8_t[IoRingFile::IORING_BUFFER_SIZE];
  if (!ac) {
    NtClose(new_handle);
    ofd->handle = nullptr;
    return -ENOMEM;
  }

  // Construct new IoRingFile at the same pool address — FILE* unchanged.
  new (ofd->file_ptr) IoRingFile(
      new_handle, buf, IoRingFile::IORING_BUFFER_SIZE,
      _IOFBF, true, modeflags, fd);
  File::add_file(ofd->file_ptr);

  *out = stream;
  return 0;
}

// ---- tmpfile_impl ----------------------------------------------------------

// Hex-encode bytes into a wide buffer (no NUL terminator).
static void hex_encode(const uint8_t *bytes, size_t count, WCHAR *out) {
  static constexpr WCHAR hex[] = u"0123456789abcdef";
  for (size_t i = 0; i < count; ++i) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 0x0F];
  }
}

intptr_t tmpfile_impl(::FILE **out) {
  // Resolve the libc-owned temp root as a DOS path with a trailing slash.
  auto temp_s = path_scratch();
  if (!temp_s) return -ENOMEM;
  WCHAR *temp_dir = temp_s.data();
  size_t dir_len = windows::get_temp_path_w(temp_dir, temp_s.size());
  if (dir_len == 0)
    return -EIO;

  // Strip trailing backslash if present.
  if (dir_len > 0 && temp_dir[dir_len - 1] == u'\\')
    --dir_len;

  // Build NT path: \??\<temp_dir>\libc_<16 hex chars>
  static constexpr size_t NAME_RANDOM_BYTES = 8;
  static constexpr size_t NAME_HEX_CHARS = NAME_RANDOM_BYTES * 2;

  auto path_s2 = path_scratch();
  if (!path_s2) return -ENOMEM;
  WCHAR *path_buf = path_s2.data();

  // Build fixed prefix: \??\<temp_dir>\libc_
  windows::WStringStream ss(cpp::span<WCHAR>(path_buf, PATH_SCRATCH_WCHARS - 1));
  ss << u"\\??\\" << windows::nt_wstring_view(temp_dir, dir_len) << u"\\libc_";
  if (ss.overflow()) return -ENAMETOOLONG;
  size_t hex_pos = ss.str().size();
  size_t pos = hex_pos + NAME_HEX_CHARS;
  if (pos + 1 > PATH_SCRATCH_WCHARS) return -ENAMETOOLONG;

  // Retry on name collision. With 64 bits of CSPRNG entropy per attempt,
  // collision is astronomically unlikely — this bound matches glibc's TMP_MAX.
  static constexpr int MAX_ATTEMPTS = 238328; // 62^3, same as glibc
  for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    uint8_t rand_bytes[NAME_RANDOM_BYTES];
    LIBC_NAMESPACE::getentropy(rand_bytes, NAME_RANDOM_BYTES);
    hex_encode(rand_bytes, NAME_RANDOM_BYTES, path_buf + hex_pos);

    OBJECT_ATTRIBUTES oa;
    windows::nt_wstring_view name(path_buf, pos);
    init_object_attributes(&oa, &name);

    // Temp files get mode 0600 — owner-only access.
    auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
    if (!sd_s)
      return -ENOMEM;
    oa.SecurityDescriptor = windows_sec::build_creation_sd(
        reinterpret_cast<UCHAR *>(sd_s.data()), 0600);

    ACCESS_MASK access =
        SYNCHRONIZE | DELETE_ACCESS | FILE_READ_ATTRIBUTES | FILE_READ_DATA |
        FILE_WRITE_DATA;
    ULONG options = FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE;
    ULONG attrs = FILE_ATTRIBUTE_TEMPORARY;

    HANDLE handle;
    NTSTATUS status = open_overlapped(
        &oa, access, FILE_CREATE, 0 /* exclusive */, &handle,
        options, attrs);

    if (status == STATUS_OBJECT_NAME_COLLISION)
      continue; // Name collision — retry with new random suffix.

    if (!NT_SUCCESS(status)) {
      if (status == STATUS_ACCESS_DENIED)
        return -EACCES;
      else
        return -EIO;
    }

    // Allocate fd. Ring + event created internally by fd_table.
    int open_flags = O_RDWR;
    auto fd_result = internal::fd_table.alloc(handle, open_flags);
    if (!fd_result.has_value()) {
      NtClose(handle);
      return -fd_result.error();
    }
    int fd = fd_result.value();
    auto *ofd = internal::fd_table.get_ofd(fd);

    // Page-aligned buffer via mmap — IoRingFile::free_file_buffer() frees
    // write_bufs with munmap, so the buffer MUST come from mmap, not new.
    // owned=false: File::close() won't delete[] it; IoRingFile handles cleanup.
    void *raw_buf = LIBC_NAMESPACE::mmap(
        nullptr, IoRingFile::IORING_BUFFER_SIZE, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw_buf == MAP_FAILED) {
      internal::fd_table.release(fd);
      NtClose(handle);
      return -ENOMEM;
    }
    uint8_t *buffer = static_cast<uint8_t *>(raw_buf);

    auto modeflags = File::ModeFlags(File::OpenMode::WRITE) |
                     File::ModeFlags(File::OpenMode::PLUS);
    void *slot = internal::file_pool::alloc();
    if (!slot) {
      LIBC_NAMESPACE::munmap(buffer, IoRingFile::IORING_BUFFER_SIZE);
      internal::fd_table.release(fd);
      NtClose(handle);
      return -ENOMEM;
    }
    auto *file = new (slot) IoRingFile(
        handle, buffer,
        IoRingFile::IORING_BUFFER_SIZE, _IOFBF, /* owned */ false,
        modeflags, fd);
    ofd->file_ptr = file;
    File::add_file(file);

    *out = reinterpret_cast<::FILE *>(file);
    return 0;
  }

  // All retries exhausted.
  return -EEXIST;
}

// ---- popen / pclose --------------------------------------------------------

// Side table mapping FILE* -> child pid for pclose. Fixed size since concurrent
// popens are rare in practice.
namespace {

struct PopenEntry {
  cpp::Atomic<::FILE *> stream;
  pid_t pid;
};

static constexpr int POPEN_TABLE_SIZE = 16;
// Zero-initialized — no global constructor needed.
// Atomic<FILE*> is trivially default-constructible when value-initialized to 0.
static PopenEntry popen_table[POPEN_TABLE_SIZE] = {};

static bool popen_table_insert(::FILE *stream, pid_t pid) {
  for (int i = 0; i < POPEN_TABLE_SIZE; ++i) {
    ::FILE *expected = nullptr;
    // Write pid before publishing the stream pointer so that a concurrent
    // pclose on another thread cannot observe a stale pid.
    popen_table[i].pid = pid;
    if (popen_table[i].stream.compare_exchange_strong(
            expected, stream, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED))
      return true;
    // CAS failed — another thread claimed this slot. The pid write is
    // harmless (they wrote their own pid before their CAS succeeded).
  }
  return false; // Table full.
}

static bool popen_table_remove(::FILE *stream, pid_t *pid_out) {
  for (int i = 0; i < POPEN_TABLE_SIZE; ++i) {
    ::FILE *expected = stream;
    if (popen_table[i].stream.compare_exchange_strong(
            expected, nullptr, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::RELAXED)) {
      *pid_out = popen_table[i].pid;
      return true;
    }
  }
  return false;
}

} // anonymous namespace

intptr_t popen_impl(const char *command, const char *mode, ::FILE **out) {
  if (!command || !mode)
    return -EINVAL;

  // Parse mode: "r" or "w" per POSIX. Also accept the glibc/musl "e"
  // extension (O_CLOEXEC on the returned FILE's fd), e.g. "re" / "we".
  bool reading;
  bool cloexec = false;
  if (mode[0] != 'r' && mode[0] != 'w')
    return -EINVAL;
  reading = (mode[0] == 'r');
  for (const char *p = mode + 1; *p; ++p) {
    if (*p == 'e')
      cloexec = true;
    else
      return -EINVAL; // Unknown flag.
  }

  // Create the pipe. Both ends get O_CLOEXEC so the child inherits only
  // the end explicitly dup2'd by the file actions.
  int pipefd[2];
  intptr_t ret = internal::pipe2(pipefd, O_CLOEXEC);
  if (ret < 0)
    return ret;

  // reading: child writes to pipefd[1] (stdout), parent reads from pipefd[0]
  // writing: child reads from pipefd[0] (stdin), parent writes to pipefd[1]
  int child_fd = reading ? pipefd[1] : pipefd[0];
  int parent_fd = reading ? pipefd[0] : pipefd[1];
  int child_target = reading ? STDOUT_FILENO : STDIN_FILENO;

  // Build file actions: dup2 the child's pipe end onto stdin or stdout.
  posix_spawn_file_actions_t actions;
  actions.__front = nullptr;
  actions.__back = nullptr;

  AllocChecker ac;
  auto *dup2_act = new (ac) SpawnFileDup2Action(child_fd, child_target);
  if (!ac) {
    internal::fd_table.release(pipefd[0]);
    internal::fd_table.release(pipefd[1]);
    return -ENOMEM;
  }
  BaseSpawnFileAction::add_action(&actions, dup2_act);

  // Also close the child's copy of the parent's end in the child.
  AllocChecker ac2;
  auto *close_act = new (ac2) SpawnFileCloseAction(parent_fd);
  if (!ac2) {
    delete dup2_act;
    internal::fd_table.release(pipefd[0]);
    internal::fd_table.release(pipefd[1]);
    return -ENOMEM;
  }
  BaseSpawnFileAction::add_action(&actions, close_act);

  // POSIX requires popen to invoke "sh -c command". Try a POSIX shell
  // first (SHELL env var, then /bin/sh). If no POSIX shell is available,
  // fall back to cmd.exe via COMSPEC.
  pid_t child_pid;
  bool used_cmd = false;

  const char *shell = internal::env_get("SHELL");
  if (!shell)
    shell = "/bin/sh";

  {
    const char *sh_argv[] = {"sh", "-c", command, nullptr};
    ret = internal::posix_spawn(&child_pid, shell, &actions, nullptr,
                                const_cast<char *const *>(sh_argv),
                                LIBC_NAMESPACE::environ);
  }

  // If the POSIX shell wasn't found, fall back to cmd.exe.
  if (ret == -ENOENT || ret == -EACCES) {
    shell = internal::env_get("COMSPEC");
    if (!shell)
      shell = "C:\\Windows\\System32\\cmd.exe";

    // Build the command line as raw text: "<shell>" /c <command>
    //
    // cmd.exe reads the raw GetCommandLineW() text; its /c switch takes the
    // entire remainder of the line as the command to execute.  Per-argument
    // quoting (as build_cmdline does) wraps /c in quotes, which cmd.exe
    // does not recognise as a switch.  Instead, we construct the command line
    // manually: the shell path is quoted (handles spaces in path), /c is
    // unquoted, and the user's command is passed verbatim.
    cpp::string_view shell_sv(shell);
    cpp::string_view cmd_sv(command);
    // Layout: '"' shell '"' ' /c ' command '\0'
    size_t raw_len = 1 + shell_sv.size() + 1 + 4 + cmd_sv.size() + 1;
    void *raw_buf =
        LIBC_NAMESPACE::mmap(nullptr, raw_len, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw_buf == MAP_FAILED) {
      delete close_act;
      delete dup2_act;
      internal::fd_table.release(pipefd[0]);
      internal::fd_table.release(pipefd[1]);
      return -ENOMEM;
    }
    char *raw_cmdline = static_cast<char *>(raw_buf);
    cpp::StringStream ss(cpp::span<char>(raw_cmdline, raw_len - 1));
    ss << '"' << shell_sv << "\" /c " << cmd_sv;
    raw_cmdline[ss.str().size()] = '\0';

    const char *cmd_argv[] = {shell, nullptr};
    ret = internal::posix_spawn(&child_pid, shell, &actions, nullptr,
                                const_cast<char *const *>(cmd_argv),
                                LIBC_NAMESPACE::environ, raw_cmdline);
    LIBC_NAMESPACE::munmap(raw_buf, raw_len);
    used_cmd = true;
  }

  (void)used_cmd;
  delete close_act;
  delete dup2_act;

  if (ret < 0) {
    internal::fd_table.release(pipefd[0]);
    internal::fd_table.release(pipefd[1]);
    return ret;
  }

  // Parent closes the child's pipe end.
  internal::fd_table.release(child_fd);

  // POSIX: "popen() shall ensure that any streams from previous popen()
  // calls that remain open in the parent process are closed in the new
  // child process." We satisfy this by always keeping O_CLOEXEC set on
  // popen pipe fds (pipe2 already set it). This matches glibc/musl.
  // The "e" mode flag is accepted for source compatibility but is
  // effectively a no-op since CLOEXEC is always on.
  (void)cloexec;

  // Pass only the base mode character to fdopen_impl ("r" or "w") —
  // it doesn't understand the "e" extension.
  const char base_mode[] = {mode[0], '\0'};

  // Convert the parent's pipe end to a FILE*.
  ::FILE *stream = nullptr;
  ret = internal::fdopen_impl(parent_fd, base_mode, &stream);
  if (ret < 0) {
    internal::fd_table.release(parent_fd);
    // Child is already running — reap it so it doesn't become a zombie.
    int discard;
    internal::waitpid(child_pid, &discard, 0);
    return ret;
  }

  // Record the FILE* -> pid association for pclose.
  if (!popen_table_insert(stream, child_pid)) {
    internal::fclose_impl(stream);
    // Reap the orphaned child.
    int discard;
    internal::waitpid(child_pid, &discard, 0);
    return -EMFILE; // Too many concurrent popens.
  }

  *out = stream;
  return 0;
}

intptr_t pclose_impl(::FILE *stream) {
  if (!stream)
    return -EINVAL;

  // Look up the child pid for this stream.
  pid_t child_pid;
  if (!popen_table_remove(stream, &child_pid))
    return -EINVAL; // Not a popen'd stream.

  // Close the pipe (flushes buffered data, sends EOF to child).
  internal::fclose_impl(stream);

  // Wait for the child to exit.
  int wstatus = 0;
  intptr_t ret = internal::waitpid(child_pid, &wstatus, 0);
  if (ret < 0)
    return ret;

  return wstatus;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
