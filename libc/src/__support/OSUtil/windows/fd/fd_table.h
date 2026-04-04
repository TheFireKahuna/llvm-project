//===-- File descriptor table for Windows ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX fd table with correct open-file-description sharing semantics.
//
// Three-level model:
//
//   FdSlot (per-descriptor)
//     -> OpenFileDescription (shared by dup/fork, refcounted)
//         -> HANDLE, position, status flags, IO Ring transport
//
// FdSlot is a single 8-byte atomic word containing:
//   - bits [47:0]  OFD pointer (canonical x86-64 userspace)
//   - bit  [48]    FD_CLOEXEC flag (per-descriptor, not shared by dup)
//   - bits [63:49] 15-bit generation counter (ABA protection)
//
// dup() bumps the OFD refcount without duplicating the kernel handle --
// two dup'd fds share the same file position, status flags, and transport.
//
// The fd table is backed by IndexedPool<FdSlot, 13> (see indexed_pool.h):
//   - Guard pages per chunk (overflow/underflow detection, MMU-enforced)
//   - Each chunk: 8192 FdSlots = 64KB (Windows allocation granularity unit)
//   - Chunks allocated on demand with guard pages, zero-initialized
//   - Chunk-level decommit when empty (physical memory returned to OS)
//   - Occupancy bitmap for tzcnt/blsr iteration (exec CLOEXEC scan)
//   - No hard limit: directory grows dynamically, no capacity ceiling
//
// Transport (IO Ring + completion event) is lazily rebuildable via a
// re-armable callonce (transport_once). After fork, transport_once is
// reset so ensure_transport() re-creates the ring on first I/O.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_TABLE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/indexed_pool.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {

// Forward declaration -- avoids circular dependency with File headers.
class File;

namespace internal {

// Forward declarations -- concrete types live in their own headers.
struct FifoChannel;
struct SocketState;
struct SocketPairChannel;
struct EpollInstance;
struct InotifyInstance;
struct FileOps;
namespace vt_pty {
struct Session;
} // namespace vt_pty

// ---------------------------------------------------------------------------
// FileKind -- discriminant for OpenFileDescription
// ---------------------------------------------------------------------------
//
// Every OFD has exactly one kind, set at creation and (except for freopen)
// immutable for its lifetime. Dispatch goes through the ops table; kind is
// used for cold-path queries (is_seekable, stat classification, etc.).
//
// Values 1-3 match Win32 FILE_TYPE_DISK/CHAR/PIPE for cheap mapping from
// GetFileType / query_file_type results.

enum class FileKind : uint8_t {
  Auto = 0,       // Sentinel: auto-detect from handle (not a real kind)
  Disk = 1,       // Seekable disk file
  Char = 2,       // Generic character device (COM port, NUL, etc.)
  Pipe = 3,       // Anonymous pipe (IoRing I/O, event-only FifoChannel)
  ConDrv = 4,     // ConDrv console (\Device\ConDrv\...)
  DevZero = 5,    // /dev/zero emulation
  Inotify = 6,    // inotify fd
  PtyMaster = 7,  // PTY master end
  PtySlave = 8,   // PTY slave end
  VirtualDir = 9, // Synthetic /dev or /dev/pts directory
  AfdSocket = 10, // AFD socket endpoint (AF_UNIX, AF_INET)
  SocketPair = 11, // Bidirectional ring buffer pair
  Epoll = 12,     // epoll instance
  Fifo = 13,      // Named FIFO (mkfifo shared-memory ring buffer)
};

// ---------------------------------------------------------------------------
// OpenFileDescription -- shared by dup/fork, pool-allocated, refcounted
// ---------------------------------------------------------------------------
//
// Layout optimized for a single 64-byte cache line:
//
//   handle + ops      (hot: every I/O dispatch)       0-15
//   refcount + flags  (warm: dup/close, fcntl)       16-27
//   _pad              (alignment)                    28-31
//   aux union         (kind-specific, 16 bytes)      32-47
//   file_ptr          (cold: fopen/fclose only)      48-55
//
// Dispatch: ofd->ops->read(ofd, buf, count). The ops pointer is set at
// creation and never changes (except freopen). Per-kind ops tables are
// constexpr, live in .rdata, and are branch-predictor-friendly.

struct OpenFileDescription {
  // --- Hot path: handle + dispatch ---

  // Underlying NT handle. Set once at creation. nullptr for synthetic fds
  // (VirtualDir, SocketPair, Fifo -- these have no kernel handle).
  HANDLE handle;

  // Per-kind operations table. Immutable after creation (except freopen).
  // Points to a const FileOps in .rdata.
  const FileOps *ops;

  // --- Lifetime ---
  cpp::Atomic<uint32_t> refcount;

  // --- Flags ---

  // Status flags shared across dup'd fds. O_APPEND, O_NONBLOCK are mutable
  // via fcntl(F_SETFL). Atomic for concurrent fcntl access.
  cpp::Atomic<int> status_flags;

  // File kind discriminant. Set at creation, immutable (except freopen).
  FileKind kind;

  // Access mode: O_RDONLY / O_WRONLY / O_RDWR. Immutable after creation.
  uint8_t access_mode;

  // Immutable open-time flags that cannot change after creation.
  //   Bit 0: O_PATH -- path-only descriptor (no I/O, only metadata and dirfd).
  static constexpr uint8_t IMMUTABLE_O_PATH = 1;
  uint8_t immutable_flags;

  uint8_t pad_ = 0;
  uint32_t pad2_ = 0;

  // --- Kind-specific auxiliary state (discriminated by `kind`) ---

  struct DiskAux {
    cpp::Atomic<int64_t> position;
    cpp::Atomic<HANDLE> section_handle;
  }; // 16 bytes

  struct PipeAux {
    FifoChannel *events;
  }; // 8 bytes

  union Aux {
    // FileKind::Disk -- seekable file with position tracking + mmap section.
    DiskAux disk;

    // FileKind::Pipe -- anonymous pipe, IoRing I/O.
    // Event-only FifoChannel for poll/epoll notification (nullable).
    PipeAux pipe;

    // FileKind::Fifo -- named FIFO (mkfifo) with shared-memory ring buffer.
    FifoChannel *fifo; // 8 bytes

    // FileKind::AfdSocket -- AFD endpoint (real socket).
    SocketState *afd; // 8 bytes

    // FileKind::SocketPair -- bidirectional ring buffer endpoint.
    // read_ch and write_ch accessible via channel->read_ch / channel->write_ch.
    SocketPairChannel *socketpair; // 8 bytes

    // FileKind::Epoll -- epoll instance.
    EpollInstance *epoll; // 8 bytes

    // FileKind::Inotify -- inotify instance.
    InotifyInstance *inotify; // 8 bytes

    // FileKind::PtyMaster, FileKind::PtySlave -- PTY session (refcounted).
    // Stored as void* to avoid exposing vt_pty::Session definition.
    void *pty_session; // 8 bytes

    // FileKind::VirtualDir -- directory classification (DevRoot or PtsDir).
    uint8_t virtual_dir_kind; // 1 byte

    // For zero-init in init_common().
    uint64_t raw[2]; // 16 bytes
  } aux;

  // FILE* pool slot. Set by fopen/fdopen, cleared by fclose.
  File *file_ptr;

  // --- Helpers (cold path, keyed on kind) ---

  LIBC_INLINE bool is_seekable() const { return kind == FileKind::Disk; }
  LIBC_INLINE bool is_pipe() const { return kind == FileKind::Pipe; }
  LIBC_INLINE bool is_console() const { return kind == FileKind::ConDrv; }
  LIBC_INLINE bool is_dev_zero() const { return kind == FileKind::DevZero; }
  LIBC_INLINE bool is_inotify() const { return kind == FileKind::Inotify; }
  LIBC_INLINE bool is_pty_master() const { return kind == FileKind::PtyMaster; }
  LIBC_INLINE bool is_pty_slave() const { return kind == FileKind::PtySlave; }
  LIBC_INLINE bool is_virtual_dir() const {
    return kind == FileKind::VirtualDir;
  }
  LIBC_INLINE bool is_path_only() const {
    return immutable_flags & IMMUTABLE_O_PATH;
  }
  LIBC_INLINE bool is_socket() const {
    return kind == FileKind::AfdSocket || kind == FileKind::SocketPair;
  }
  LIBC_INLINE bool is_fifo() const { return kind == FileKind::Fifo; }
  LIBC_INLINE bool is_epoll() const { return kind == FileKind::Epoll; }

  // --- Typed aux accessors (assert-guarded) ---

  LIBC_INLINE DiskAux &disk() {
    LIBC_ASSERT(kind == FileKind::Disk);
    return aux.disk;
  }
  LIBC_INLINE const DiskAux &disk() const {
    LIBC_ASSERT(kind == FileKind::Disk);
    return aux.disk;
  }
  LIBC_INLINE FifoChannel *pipe_events() const {
    LIBC_ASSERT(kind == FileKind::Pipe);
    return aux.pipe.events;
  }
  // Non-asserting variant for callers that dispatch across multiple kinds
  // (e.g., poll_ops where Pipe and Fifo share a code path).
  LIBC_INLINE FifoChannel *pipe_events_or_null() const {
    return kind == FileKind::Pipe ? aux.pipe.events : nullptr;
  }
  LIBC_INLINE FifoChannel *fifo_channel() const {
    LIBC_ASSERT(kind == FileKind::Fifo);
    return aux.fifo;
  }
  LIBC_INLINE SocketState *afd_socket() const {
    LIBC_ASSERT(kind == FileKind::AfdSocket);
    return aux.afd;
  }
  LIBC_INLINE SocketPairChannel *socket_pair() const {
    LIBC_ASSERT(kind == FileKind::SocketPair);
    return aux.socketpair;
  }
  LIBC_INLINE EpollInstance *epoll_inst() const {
    LIBC_ASSERT(kind == FileKind::Epoll);
    return aux.epoll;
  }
  LIBC_INLINE InotifyInstance *inotify_inst() const {
    LIBC_ASSERT(kind == FileKind::Inotify);
    return aux.inotify;
  }
  LIBC_INLINE void *pty_session() const {
    LIBC_ASSERT(kind == FileKind::PtyMaster || kind == FileKind::PtySlave);
    return aux.pty_session;
  }

  // --- Typed aux setters (assert-guarded, for init sites) ---

  LIBC_INLINE void set_pipe_events(FifoChannel *ch) {
    LIBC_ASSERT(kind == FileKind::Pipe);
    aux.pipe.events = ch;
  }
  LIBC_INLINE void set_fifo_channel(FifoChannel *ch) {
    LIBC_ASSERT(kind == FileKind::Fifo);
    aux.fifo = ch;
  }
  LIBC_INLINE void set_afd_socket(SocketState *s) {
    LIBC_ASSERT(kind == FileKind::AfdSocket);
    aux.afd = s;
  }
  LIBC_INLINE void set_socket_pair(SocketPairChannel *sp) {
    LIBC_ASSERT(kind == FileKind::SocketPair);
    aux.socketpair = sp;
  }
  LIBC_INLINE void set_epoll_inst(EpollInstance *inst) {
    LIBC_ASSERT(kind == FileKind::Epoll);
    aux.epoll = inst;
  }
  LIBC_INLINE void set_inotify_inst(InotifyInstance *inst) {
    LIBC_ASSERT(kind == FileKind::Inotify);
    aux.inotify = inst;
  }
  LIBC_INLINE void set_pty_session(void *session) {
    LIBC_ASSERT(kind == FileKind::PtyMaster || kind == FileKind::PtySlave);
    aux.pty_session = session;
  }
  LIBC_INLINE void set_virtual_dir_kind(uint8_t dk) {
    LIBC_ASSERT(kind == FileKind::VirtualDir);
    aux.virtual_dir_kind = dk;
  }
  LIBC_INLINE uint8_t get_virtual_dir_kind() const {
    LIBC_ASSERT(kind == FileKind::VirtualDir);
    return aux.virtual_dir_kind;
  }

  // Initialize common fields. Called once after pool allocation.
  // Kind-specific aux fields must be set by the caller via typed setters.
  void init_common(HANDLE h, int open_flags, FileKind k, const FileOps *o);

  // Reinitialize for freopen: resets handle, kind, ops, aux, access_mode,
  // status_flags, and immutable_flags — but preserves refcount and file_ptr
  // (the OFD is still live with existing dup'd references and a FILE*).
  // Caller must call ops->release_aux() on the OLD identity before calling.
  void reinit_for_reopen(HANDLE h, int open_flags, FileKind k,
                         const FileOps *o);

  // Decrement refcount. If last reference, call ops->release_aux(),
  // close the handle, and return the OFD to the pool.
  void release();
};

static_assert(sizeof(OpenFileDescription) == 56,
              "OpenFileDescription must be exactly 56 bytes");

// ---------------------------------------------------------------------------
// Tagged pointer -- OFD pointer + CLOEXEC + generation in one atomic word
// ---------------------------------------------------------------------------
//
// x86-64 canonical addressing: userspace pointers have bits [63:47] = 0.
// We pack per-fd state into the unused upper bits:
//
//   bits [63:49]  15-bit generation counter (32,768 before wrap)
//   bit  [48]     FD_CLOEXEC flag
//   bits [47:0]   OFD pointer (canonical userspace)
//
// 15-bit generation is sufficient: ABA would require 32K close/reopen
// cycles on the exact same fd while another thread holds a stale
// reference mid-operation.
//
// Packing CLOEXEC here eliminates the separate fd_flags atomic,
// shrinking FdSlot from 16 to 8 bytes. fcntl(F_SETFD) becomes a
// CAS loop (cold path), but all reads are a single atomic load.

struct TaggedOfd {
  static constexpr unsigned PTR_BITS = 48;
  static constexpr unsigned CLOEXEC_BIT = 48;
  static constexpr unsigned GEN_SHIFT = 49;
  static constexpr unsigned GEN_BITS = 15;

  static constexpr uintptr_t PTR_MASK = (1ULL << PTR_BITS) - 1;
  static constexpr uintptr_t CLOEXEC_MASK = 1ULL << CLOEXEC_BIT;
  static constexpr uintptr_t GEN_ONE = 1ULL << GEN_SHIFT;
  static constexpr uintptr_t GEN_MASK = (1ULL << GEN_BITS) - 1; // 0x7FFF
  static constexpr uintptr_t TAG_MASK = ~PTR_MASK; // bits [63:48]

  uintptr_t bits;

  LIBC_INLINE static TaggedOfd make(OpenFileDescription *ofd, uint16_t gen,
                                    bool cloexec = false) {
    // Mask gen to GEN_BITS to prevent UB when gen wraps past 15 bits.
    // Without this, (gen << 49) with gen >= 0x8000 would shift into or
    // past bit 64, which is undefined behavior on a 64-bit type.
    return {(static_cast<uintptr_t>(gen & GEN_MASK) << GEN_SHIFT) |
            (cloexec ? CLOEXEC_MASK : 0) |
            (reinterpret_cast<uintptr_t>(ofd) & PTR_MASK)};
  }

  LIBC_INLINE static TaggedOfd null() { return {0}; }

  LIBC_INLINE OpenFileDescription *ptr() const {
    return reinterpret_cast<OpenFileDescription *>(bits & PTR_MASK);
  }

  LIBC_INLINE uint16_t gen() const {
    return static_cast<uint16_t>(bits >> GEN_SHIFT);
  }

  LIBC_INLINE bool cloexec() const { return (bits & CLOEXEC_MASK) != 0; }

  LIBC_INLINE TaggedOfd with_ptr(OpenFileDescription *ofd) const {
    return {(bits & TAG_MASK) |
            (reinterpret_cast<uintptr_t>(ofd) & PTR_MASK)};
  }

  LIBC_INLINE TaggedOfd with_cloexec(bool set) const {
    return set ? TaggedOfd{bits | CLOEXEC_MASK}
               : TaggedOfd{bits & ~CLOEXEC_MASK};
  }

  // Bump generation, clear pointer and CLOEXEC. Used by close().
  LIBC_INLINE TaggedOfd next_gen_null() const {
    return {(bits + GEN_ONE) & ~(PTR_MASK | CLOEXEC_MASK)};
  }

  LIBC_INLINE bool operator==(TaggedOfd other) const {
    return bits == other.bits;
  }
  LIBC_INLINE bool operator!=(TaggedOfd other) const {
    return bits != other.bits;
  }
};

// ---------------------------------------------------------------------------
// FdSlot -- per-descriptor, lives in the fd_table chunk array
// ---------------------------------------------------------------------------

struct FdSlot {
  // Single 8-byte atomic word:
  //   OFD pointer + CLOEXEC flag + generation counter.
  // nullptr (bits == 0 modulo tag) = free slot.
  // A single CAS handles allocation, release, CLOEXEC, and ABA detection.
  cpp::Atomic<uintptr_t> tagged_ofd;

  // Load the tagged pointer atomically.
  LIBC_INLINE TaggedOfd load_tagged(cpp::MemoryOrder order) const {
    return {const_cast<cpp::Atomic<uintptr_t> &>(tagged_ofd).load(order)};
  }

  // Store the tagged pointer atomically.
  LIBC_INLINE void store_tagged(TaggedOfd t, cpp::MemoryOrder order) {
    tagged_ofd.store(t.bits, order);
  }

  // CAS the tagged pointer. Returns true on success, updates expected on
  // failure.
  LIBC_INLINE bool cas_tagged(TaggedOfd &expected, TaggedOfd desired,
                              cpp::MemoryOrder order) {
    return tagged_ofd.compare_exchange_strong(expected.bits, desired.bits,
                                              order);
  }

  // Convenience: load just the OFD pointer.
  LIBC_INLINE OpenFileDescription *load_ofd(cpp::MemoryOrder order) const {
    return load_tagged(order).ptr();
  }

  // Read FD_CLOEXEC. Single atomic load.
  LIBC_INLINE bool cloexec(
      cpp::MemoryOrder order = cpp::MemoryOrder::ACQUIRE) const {
    return load_tagged(order).cloexec();
  }

  // Set/clear FD_CLOEXEC via CAS loop. Cold path (fcntl only).
  LIBC_INLINE void set_cloexec(
      bool set, cpp::MemoryOrder order = cpp::MemoryOrder::ACQ_REL) {
    TaggedOfd current = load_tagged(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
      TaggedOfd desired = current.with_cloexec(set);
      if (current == desired)
        return; // Already in desired state.
      if (cas_tagged(current, desired, order))
        return;
      // CAS failed -- current was updated. Retry.
    }
  }
};

// FdSlot is exactly 8 bytes: one atomic word.
// Down from 16 bytes in the old (tagged_ofd + fd_flags) design.
static_assert(sizeof(FdSlot) == 8, "FdSlot must be exactly 8 bytes");

// ---------------------------------------------------------------------------
// FdTable -- process-wide singleton
// ---------------------------------------------------------------------------
//
// Backed by IndexedPool<FdSlot, 13> which provides:
//   - Guard pages per chunk (MMU-enforced overflow/underflow detection)
//   - Chunk-level decommit when empty (physical memory returned to OS)
//   - Recommit on demand (zero-filled by OS, all slots appear free)
//   - Atomic occupancy bitmap (tzcnt/blsr iteration for exec CLOEXEC)
//   - Growable directory with no hard limit
//   - Lock-free hot path (2 atomic loads)

class FdTable {
public:
  // Pool type: 8192 FdSlots per chunk (64KB = Windows alloc granularity).
  using Pool = IndexedPool<FdSlot, 13>;

  // Chunk geometry (forwarded from pool for callers that need them).
  static constexpr unsigned CHUNK_SHIFT = Pool::CHUNK_SHIFT;
  static constexpr unsigned CHUNK_SLOTS = Pool::SLOTS_PER_CHUNK;
  static constexpr unsigned CHUNK_MASK = Pool::CHUNK_MASK;

private:
  Pool pool_;                           // chunk management + guard pages + bitmap
  cpp::Atomic<int> alloc_hint_;         // lowest possibly-free fd (scan start)
  cpp::Atomic<int> high_water_;         // highest fd ever allocated + 1

  // Hot path: look up slot for fd. Returns nullptr if chunk not allocated.
  // Only call for known-live fds (the chunk's data pages must be committed).
  LIBC_INLINE FdSlot *slot_for(int fd) {
    if (fd < 0)
      return nullptr;
    return pool_.slot_for(static_cast<unsigned>(fd));
  }

  // Ensure chunk exists and is committed for fd. Returns nullptr on OOM.
  LIBC_INLINE FdSlot *ensure_slot(int fd) {
    if (fd < 0)
      return nullptr;
    return pool_.ensure_slot(static_cast<unsigned>(fd));
  }

  // Allocate a free FdSlot via CAS on the tagged pointer. Returns fd number.
  // Chunk-aware: scans within each chunk after a single ensure_chunk call.
  ErrorOr<int> alloc_slot(OpenFileDescription *ofd, int min_fd,
                          bool cloexec = false);

  // Release a slot: CAS tagged pointer to next-gen null.
  // Returns the old OFD (caller handles refcount/cleanup).
  OpenFileDescription *release_slot(int fd);

  // Bind one of the three standard descriptors to a live process std handle.
  void bind_std_fd(int fd, HANDLE handle);
  void bind_std_fd_known_type(int fd, HANDLE handle, FileKind known_kind);

public:
  // Classify a handle as disk / char / pipe via native NT volume metadata.
  static DWORD query_file_type(HANDLE handle);

  // Map a Win32 file type + optional ConDrv probe to FileKind.
  static FileKind classify_handle(HANDLE h, DWORD ftype);

  // Initialize: set up pool, allocate chunk 0. Called once at startup
  // (fd_table_startup_init(), Phase 6).
  void init();

  // Rebind fd 0/1/2 to the supplied process std handles without taking
  // ownership of any displaced handles. This is used when a process publishes
  // or restores a console session after startup.
  void rebind_std_fds(HANDLE std_input, HANDLE std_output, HANDLE std_error);
  void rebind_std_fds_as_console(HANDLE std_input, HANDLE std_output,
                                 HANDLE std_error);

  // Allocate the lowest available fd >= min_fd. Creates an OFD from the
  // pool, sets up transport. Returns fd on success, ENOMEM if out of memory.
  // When kind_hint is non-zero, it overrides the auto-detected file kind.
  ErrorOr<int> alloc(HANDLE h, int open_flags, int min_fd = 0,
                     FileKind kind_hint = FileKind::Auto);
  // aux_byte: written to aux.raw[0] (as a uint8_t at offset 0) before the
  // slot is published. Used by VirtualDir to set the dir_kind atomically
  // with slot creation, avoiding a TOCTOU window.
  ErrorOr<int> alloc_synthetic(FileKind kind, int open_flags, int min_fd = 0,
                               uint8_t aux_byte = 0);

  // Allocate a specific fd (for dup2). Releases any existing OFD at that
  // slot first.
  ErrorOr<int> alloc_at(int fd, HANDLE h, int open_flags);

  // Duplicate old_fd to the lowest available fd >= min_fd.
  // Shares the OFD (refcount bump) -- no NtDuplicateObject, no new IO Ring.
  ErrorOr<int> dup(int old_fd, int min_fd = 0, int new_fd_flags = 0);

  // Duplicate old_fd to a specific new_fd (dup2/dup3 semantics).
  // Releases any existing OFD at new_fd first.
  ErrorOr<int> dup_to(int old_fd, int new_fd, int new_fd_flags = 0);

  // Release an fd. Decrements OFD refcount; closes resources only when
  // the last reference is released.
  ErrorOr<int> release(int fd);

  // Mark an fd slot as free without closing resources. Used after
  // platform_close has already closed the handle/ring/event (fclose path).
  void free_slot(int fd);

  // Look up an fd. Returns the HANDLE, or Error(EBADF) if invalid.
  ErrorOr<HANDLE> get(int fd);

  // Get the OFD for an fd. Returns nullptr if fd is invalid or free.
  OpenFileDescription *get_ofd(int fd);

  // Get the FdSlot for an fd (for cloexec access). Returns nullptr if
  // fd is out of range or chunk not allocated.
  FdSlot *get_slot(int fd);

  // Set or clear FD_CLOEXEC on an fd and synchronize the kernel handle's
  // OBJ_INHERIT flag. POSIX: CLOEXEC means non-inheritable; no CLOEXEC
  // means inheritable. Handles without an underlying NT handle (synthetic
  // fds like FIFO, SocketPair, VirtualDir) only update the slot bit.
  void set_fd_cloexec(int fd, bool cloexec);

  // Upper bound for fd scanning (exec CLOEXEC, etc.). One past the highest
  // fd ever allocated. Conservative -- may overcount if high fds are closed.
  int high_water() {
    return high_water_.load(cpp::MemoryOrder::ACQUIRE);
  }

  // Bitmap-based iteration of all live fd slots. Uses tzcnt/blsr to skip
  // 64 empty slots per instruction. Skips decommitted (dormant) chunks.
  // Callback receives fd number, slot pointer, and caller context.
  using FdCallback = void (*)(int fd, FdSlot *slot, void *ctx);
  void for_each_live(FdCallback cb, void *ctx);

  // Seed fd 0/1/2 from PEB and construct their File objects.
  // Called once from startup code (fd_table_std_fds_startup_init(), Phase 8).
  // Requires ofd_pool::init() and file_pool::init() to have been called.
  void init_std_fds();

  // Fork child reinit: reset pool locks, decommit empty chunks,
  // per-thread IO rings don't survive fork (lazily recreated).
  void fork_reinit();
};

// Process-wide singleton.
extern FdTable fd_table;

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FD_TABLE_H
