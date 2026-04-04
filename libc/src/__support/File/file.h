//===--- A platform independent file data structure -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FILE_FILE_H
#define LLVM_LIBC_SRC___SUPPORT_FILE_FILE_H

#include "hdr/stdint_proxy.h"
#include "hdr/stdio_macros.h"
#include "hdr/types/off_t.h"
#include "src/__support/CPP/new.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/architectures.h"
#include "src/__support/threads/mutex.h"

#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
#include "src/__support/wchar/mbstate.h"
#include "hdr/types/wchar_t.h"
#endif

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {

// FileIOResult is defined in file_io_result.h (included above) so that
// low-level platform helpers (e.g. IO Ring) can use it without pulling in
// the full File class.

// This a generic base class to encapsulate a platform independent file data
// structure. Platform specific specializations should create a subclass as
// suitable for their platform.
class File {
public:
  static void add_file(File *f);
  static void remove_file(File *f);
  static File *get_first_file();
  static void lock_list();
  static void unlock_list();

  // Flush all open streams. Called by exit() to satisfy C11 7.22.4.4p4.
  static void flush_all();

  static File *list_all;
  static Mutex list_lock;

  LIBC_INLINE File *get_next() const { return next; }
  LIBC_INLINE File *get_prev() const { return prev; }
  LIBC_INLINE void set_next(File *f) { next = f; }
  LIBC_INLINE void set_prev(File *f) { prev = f; }

  static constexpr size_t DEFAULT_BUFFER_SIZE = 1024;

  using LockFunc = void(File *);
  using UnlockFunc = void(File *);

  using WriteFunc = FileIOResult(File *, const void *, size_t);
  using ReadFunc = FileIOResult(File *, void *, size_t);
  // The SeekFunc is expected to return the current offset of the external
  // file position indicator.
  using SeekFunc = ErrorOr<off_t>(File *, off_t, int);
  using CloseFunc = int(File *);
  // Called after flush_unlocked writes buffered data. Implementations use
  // this to drain asynchronous write-behind pipelines. Returns errno or 0.
  using SyncFunc = int(File *);

  using ModeFlags = uint32_t;

  // The three different types of flags below are to be used with '|' operator.
  // Their values correspond to mutually exclusive bits in a 32-bit unsigned
  // integer value. A flag set can include both READ and WRITE if the file
  // is opened in update mode (ie. if the file was opened with a '+' the mode
  // string.)
  enum class OpenMode : ModeFlags {
    READ = 0x1,
    WRITE = 0x2,
    APPEND = 0x4,
    PLUS = 0x8,
  };

  // Denotes a file opened in binary mode (which is specified by including
  // the 'b' character in teh mode string.)
  enum class ContentType : ModeFlags {
    BINARY = 0x10,
  };

  // Denotes a file to be created for writing.
  enum class CreateType : ModeFlags {
    EXCLUSIVE = 0x100,
  };

private:
  enum class FileOp : uint8_t { NONE, READ, WRITE, SEEK };

  // Platform specific functions which create new file objects should initialize
  // these fields suitably via the constructor. Typically, they should be simple
  // syscall wrappers for the corresponding functionality.
  WriteFunc *platform_write;
  ReadFunc *platform_read;
  SeekFunc *platform_seek;
  CloseFunc *platform_close;
  SyncFunc *platform_sync; // nullptr = no-op (no write-behind to drain).

  Mutex mutex;

  // For files which are readable, we should be able to support one ungetc
  // operation even if |buf| is nullptr. So, in the constructor of File, we
  // set |buf| to point to this buffer character.
  uint8_t ungetc_buf;

  uint8_t *buf;   // Pointer to the stream buffer for buffered streams
  size_t bufsize; // Size of the buffer pointed to by |buf|.

  // Buffering mode to used to buffer.
  int bufmode;

  // If own_buf is true, the |buf| is owned by the stream and will be
  // free-ed when close method is called on the stream.
  bool own_buf;

  // The mode in which the file was opened.
  ModeFlags mode;

  // Current read or write pointer.
  size_t pos;

  // Represents the previous operation that was performed.
  FileOp prev_op;

  // When the buffer is used as a read buffer, read_limit is the upper limit
  // of the index to which the buffer can be read until.
  size_t read_limit;

  bool eof;
  bool err;

  File *prev;
  File *next;

#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
  // Stream orientation per C11 §7.21.2: 0 = unset, >0 = wide, <0 = byte.
  // Set by the first byte or wide I/O operation; only freopen clears it.
  int orientation;

  // Conversion state for wide-oriented streams (C11 §7.21.2).
  // Tracks partial multibyte sequences across fputwc/fgetwc calls.
  // Reset on seek and freopen.
  internal::mbstate wide_mbstate;

  // Wide pushback slot for ungetwc (C11 §7.29.3.10). Separate from the
  // byte buffer because the buffer holds encoded UTF-8, not wchar_t.
  // Cleared by seek and freopen.
  bool has_wide_pushback;
  wchar_t wide_pushback_buf;
#endif

  // This is a convenience RAII class to lock and unlock file objects.
  class FileLock {
    File *file;

  public:
    explicit FileLock(File *f) : file(f) { file->lock(); }

    ~FileLock() { file->unlock(); }

    FileLock(const FileLock &) = delete;
    FileLock(FileLock &&) = delete;
  };

protected:
  // Allows platform subclasses to redirect the stream buffer (e.g. for
  // double-buffered write-behind). The buffer size is unchanged.
  void set_buffer_ptr(uint8_t *new_buf) { buf = new_buf; }
  uint8_t *get_buffer_ptr() const { return buf; }

  constexpr bool write_allowed() const {
    return mode & (static_cast<ModeFlags>(OpenMode::WRITE) |
                   static_cast<ModeFlags>(OpenMode::APPEND) |
                   static_cast<ModeFlags>(OpenMode::PLUS));
  }

  constexpr bool read_allowed() const {
    return mode & (static_cast<ModeFlags>(OpenMode::READ) |
                   static_cast<ModeFlags>(OpenMode::PLUS));
  }

  constexpr bool is_append_mode() const {
    return mode & static_cast<ModeFlags>(OpenMode::APPEND);
  }

  constexpr ModeFlags get_mode_flags() const { return mode; }

  void set_mode_flags(ModeFlags modeflags) { mode = modeflags; }

public:
  // We want this constructor to be constexpr so that global file objects
  // like stdout do not require invocation of the constructor which can
  // potentially lead to static initialization order fiasco. Consequently,
  // we will assume that the |buffer| and |buffer_size| argument are
  // meaningful - that is, |buffer| is nullptr if and only if |buffer_size|
  // is zero. This way, we will not have to employ the semantics of
  // the set_buffer method and allocate a buffer.
  constexpr File(WriteFunc *wf, ReadFunc *rf, SeekFunc *sf, CloseFunc *cf,
                 uint8_t *buffer, size_t buffer_size, int buffer_mode,
                 bool owned, ModeFlags modeflags, SyncFunc *syncf = nullptr)
      : platform_write(wf), platform_read(rf), platform_seek(sf),
        platform_close(cf), platform_sync(syncf),
        mutex(/*timed=*/false, /*recursive=*/true,
              /*robust=*/false, /*pshared=*/false),
        ungetc_buf(0), buf(buffer), bufsize(buffer_size), bufmode(buffer_mode),
        own_buf(owned), mode(modeflags), pos(0), prev_op(FileOp::NONE),
        read_limit(0), eof(false), err(false), prev(nullptr), next(nullptr)
#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
        , orientation(0), wide_mbstate{}, has_wide_pushback(false),
        wide_pushback_buf(0)
#endif
  {
    adjust_buf();
  }

  // Buffered write of |len| bytes from |data| without the file lock.
  FileIOResult write_unlocked(const void *data, size_t len);

  // Buffered write of |len| bytes from |data| under the file lock.
  FileIOResult write(const void *data, size_t len) {
    FileLock l(this);
    return write_unlocked(data, len);
  }

  // Buffered read of |len| bytes into |data| without the file lock.
  FileIOResult read_unlocked(void *data, size_t len);

  // Buffered read of |len| bytes into |data| under the file lock.
  FileIOResult read(void *data, size_t len) {
    FileLock l(this);
    return read_unlocked(data, len);
  }

  // Callers holding the file lock use the *_unlocked variants to avoid the
  // recursive re-entry when they need to pair a seek or tell with other
  // state reads/writes (e.g. fgetpos/fsetpos snapshotting mbstate).
  ErrorOr<int> seek_unlocked(off_t offset, int whence);
  ErrorOr<int> seek(off_t offset, int whence);

  ErrorOr<off_t> tell_unlocked();
  ErrorOr<off_t> tell();

  // If buffer has data written to it, flush it out. Does nothing if the
  // buffer is currently being used as a read buffer.
  int flush() {
    FileLock lock(this);
    return flush_unlocked();
  }

  int flush_unlocked();

  // Returns EOF on error and keeps the file unchanged.
  int ungetc_unlocked(int c);

  int ungetc(int c) {
    FileLock lock(this);
    return ungetc_unlocked(c);
  }

  // Flushes buffered data (including async write-behind via platform_sync),
  // frees owned buffers, then calls platform_close to release platform
  // resources. POSIX requires fclose to close the file even if flush fails,
  // so we always call platform_close and return the first error.
  int close() {
    // Unlink from the global file list BEFORE acquiring the per-file mutex.
    // This enforces a consistent lock ordering (list_lock → per-file mutex)
    // shared with fflush(NULL), which holds list_lock while calling
    // f->flush() on each file. The previous order (per-file mutex in
    // FileLock, then list_lock in remove_file via platform_close) inverted
    // this and caused ABBA deadlocks under concurrent fclose + fflush(NULL).
    //
    // POSIX §7.21.5.1: after fclose begins, any concurrent use of the
    // stream is undefined behavior, so fflush(NULL) skipping a stream
    // that is mid-close is correct — it is no longer an "open" stream.
    File::remove_file(this);

    int flush_err;
    {
      FileLock lock(this);
      flush_err = flush_unlocked();
    }

    // Free owned buffer before platform_close destroys the File object.
    // Platforms using non-heap buffers (e.g. mmap) set owned=false and
    // handle their own buffer cleanup in platform_close.
    if (own_buf)
      delete[] buf;

    // Platform close is expected to cleanup the file data structure which
    // includes the file mutex. Hence, we call platform_close after releasing
    // the file lock. Another thread doing file operations while a thread is
    // closing the file is undefined behavior as per POSIX.
    int close_err = platform_close(this);
    return flush_err ? flush_err : close_err;
  }

  // Sets the internal buffer to |buffer| with buffering mode |mode|.
  // |size| is the size of |buffer|. If |size| is non-zero, but |buffer|
  // is nullptr, then a buffer owned by this file will be allocated.
  // Else, |buffer| will not be owned by this file.
  //
  // Will return zero on success, or an error value on failure. Will fail
  // if:
  //   1. |buffer| is not a nullptr but |size| is zero.
  //   2. |buffer_mode| is not one of _IOLBF, IOFBF or _IONBF.
  //   3. If an allocation was required but the allocation failed.
  // For cases 1 and 2, the error returned in EINVAL. For case 3, error returned
  // is ENOMEM.
  int set_buffer(void *buffer, size_t size, int buffer_mode);

  void lock() { mutex.lock(); }
  void unlock() { mutex.unlock(); }

  // Non-blocking lock attempt for fflush(NULL). Returns true if the lock
  // was acquired. Used to avoid ABBA deadlock: fflush(NULL) holds list_lock
  // and must not block on a per-file mutex that a concurrent fclose holder
  // might need list_lock to release.
  bool try_lock_for_flush() { return mutex.try_lock() == MutexError::NONE; }

  bool error_unlocked() const { return err; }

  void set_err_unlocked() { err = true; }

  bool error() {
    FileLock l(this);
    return error_unlocked();
  }

  // TODO: https://github.com/llvm/llvm-project/issues/172302
  // MacOS defines clearerr_unlocked as a macro. While pre-processing, the
  // identifier below is substituted for the definition in the SDK, which leads
  // to compile time errors due to ill-formed statements. This is a workaround
  // for the pre-processor.
#pragma push_macro("clearerr_unlocked")
#undef clearerr_unlocked
  void clearerr_unlocked() {
    err = false;
    eof = false;
  }
#pragma pop_macro("clearerr_unlocked")

  void clearerr() {
    FileLock l(this);
    clearerr_unlocked();
  }

  bool iseof_unlocked() { return eof; }

  bool iseof() {
    FileLock l(this);
    return iseof_unlocked();
  }

#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
  // C11 §7.29.3.5: query or set stream orientation.
  // Returns >0 for wide, <0 for byte, 0 for unset.
  int fwide_unlocked(int mode) {
    if (mode && !orientation)
      orientation = mode > 0 ? 1 : -1;
    return orientation;
  }

  int fwide(int mode) {
    FileLock l(this);
    return fwide_unlocked(mode);
  }

  // Called by wide/byte I/O entry points on first byte of the op. Adopts
  // the requested orientation on an unoriented stream and returns true; on
  // a stream already oriented the other way returns false so the caller
  // can bail. C11 §7.21.2p3 makes byte-on-wide and wide-on-byte undefined
  // behavior — rejecting avoids silent corruption of wide_mbstate or the
  // byte buffer. `mode` must be non-zero.
  bool adopt_orientation_unlocked(int mode) {
    if (!orientation) {
      orientation = mode > 0 ? 1 : -1;
      return true;
    }
    return (orientation > 0) == (mode > 0);
  }

  // Reset stream orientation and conversion state (called by freopen).
  void reset_orientation() {
    orientation = 0;
    wide_mbstate = internal::mbstate{};
    has_wide_pushback = false;
  }

  // Accessor for wide I/O functions that need the per-stream conversion state.
  internal::mbstate *get_wide_mbstate() { return &wide_mbstate; }

  // Wide pushback slot access for ungetwc / fgetwc.
  bool has_wide_unget() const { return has_wide_pushback; }

  bool push_wide_char(wchar_t wc) {
    if (has_wide_pushback)
      return false;
    wide_pushback_buf = wc;
    has_wide_pushback = true;
    eof = false; // C11 §7.29.3.10: clears the EOF indicator.
    return true;
  }

  wchar_t pop_wide_char() {
    wchar_t wc = wide_pushback_buf;
    has_wide_pushback = false;
    wide_pushback_buf = 0;
    return wc;
  }

  void clear_wide_unget() { has_wide_pushback = false; }
#endif // LIBC_COPT_FILE_WIDE_SUPPORT

  // Returns an bit map of flags corresponding to enumerations of
  // OpenMode, ContentType and CreateType.
  static ModeFlags mode_flags(const char *mode);

private:
  FileIOResult write_unlocked_lbf(const uint8_t *data, size_t len);
  FileIOResult write_unlocked_fbf(const uint8_t *data, size_t len);
  FileIOResult write_unlocked_nbf(const uint8_t *data, size_t len);

  FileIOResult read_unlocked_fbf(uint8_t *data, size_t len);
  FileIOResult read_unlocked_nbf(uint8_t *data, size_t len);
  size_t copy_data_from_buf(uint8_t *data, size_t len);

  constexpr void adjust_buf() {
    if (read_allowed() && (buf == nullptr || bufsize == 0)) {
      // We should allow atleast one ungetc operation.
      // This might give an impression that a buffer will be used even when
      // the user does not want a buffer. But, that will not be the case.
      // For reading, the buffering does not come into play. For writing, let
      // us take up the three different kinds of buffering separately:
      // 1. If user wants _IOFBF but gives a zero buffer, buffering still
      //    happens in the OS layer until the user flushes. So, from the user's
      //    point of view, this single byte buffer does not affect their
      //    experience.
      // 2. If user wants _IOLBF but gives a zero buffer, the reasoning is
      //    very similar to the _IOFBF case.
      // 3. If user wants _IONBF, then the buffer is ignored for writing.
      // So, all of the above cases, having a single ungetc buffer does not
      // affect the behavior experienced by the user.
      buf = &ungetc_buf;
      bufsize = 1;
    }
  }
};

// The implementation of this function is provided by the platform_file
// library.
ErrorOr<File *> openfile(const char *path, const char *mode);

// The platform_file library should implement it if it relevant for that
// platform.
int get_fileno(File *f);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_FILE_FILE_H
