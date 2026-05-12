//===-- MAP_PRIVATE file CoW stress for the Windows mmap subsystem --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Stress path for file-backed Copy-on-Write on the unified region/shape
// pipeline — the only test that exercises MAP_PRIVATE on a real disk file
// (not anonymous, not shared) and the legacy region/shape teardown machinery
// it sits on top of. Note: this test targets the legacy mmap engines in
// `memory/legacy/`; the comment block below names legacy mechanism IDs
// (MONO/CHUNKED, RegionDesc, CowContext, commit_remap) verbatim until the
// Layer 8 P3 cutover rewires the test to typed va_tracker ops.
//
//   * MAP_PRIVATE on a real disk file (not anonymous, not shared).
//   * `RegionDesc.flags & REGION_FLAG_COW` must drive `CowContext::save_cow_pages`
//     during partial_unmap_view of a `FILE_VIEW_MONO` region — without that,
//     the dirty CoW data on the surviving fragments would be lost when the
//     section view is unmapped during the MONO→CHUNKED promotion.
//   * The MONO→CHUNKED shape promotion must serialize cleanly under multiple
//     concurrent partial-unmap punches into the same region.
//   * The `commit_remap` rollback path must keep the surviving fragment(s)
//     and the underlying section refcount consistent — a silent slot drop
//     during rollback strands the CoW context and leaks the section refcount.
//   * The backing file content is a separate witness: CoW writes must never
//     leak into the file regardless of the partial-unmap shape we drive.
//
// Five phases, modelled as separate TEST_F bodies under one fixture so that
// each can fail independently without masking the others. The original
// pre-redesign test was structured as escalating phases; the new shape model
// makes that structure align well with the natural state-transition coverage:
//
//   Phase1 — single-thread baseline CoW sanity (no shape promotion).
//   Phase2 — single-thread partial-unmap; pins MONO→CHUNKED + CowContext.
//   Phase3 — multi-thread partial-unmaps into one region; pins shape promotion
//            serialization + chunk_list parking + per-chunk CoW preservation.
//   Phase4 — drive CHUNKED → empty, validating per-chunk teardown and the
//            section refcount returning to zero (no leaks; file unchanged).
//   Phase5 — multi-thread mixed lifecycle churn with a working-set bound,
//            catching cumulative leaks in the region pool / placeholder VA.
//
//===----------------------------------------------------------------------===//

#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/munmap.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"
#include "test/src/__support/windows/nt_test_utils.h"

#include <initializer_list>
#include <stddef.h>
#include <stdint.h>

using LIBC_NAMESPACE::cpp::Atomic;
using LIBC_NAMESPACE::cpp::MemoryOrder;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMmapPrivateFileStressTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

// ---------------------------------------------------------------------------
// Constants — the file is laid out in 64K units to align with NT's section
// granularity and the radix-tree slot stride. CoW is page-granular underneath,
// but every shape-relevant decision (chunk boundaries, placeholder splits)
// happens at 64K, so the test exercises that grain too.
// ---------------------------------------------------------------------------

static constexpr size_t kAllocGranularity = 64 * 1024;
static constexpr ULONG kPagePattern = 0xA5A5'5A5Au;

// ---------------------------------------------------------------------------
// File helpers — go through NtCreateFile directly. We deliberately do NOT
// route through libc open()/close() because:
//   1. The test must read the backing file *as a separate handle* to verify
//      the CoW pages did not leak through to disk; bypassing the libc fd
//      table on the verification side keeps the witness independent of the
//      object under test.
//   2. mmap() in this libc accepts both fd and HANDLE-derived paths; we need
//      the HANDLE form for FILE_DELETE_ON_CLOSE cleanup which keeps the test
//      hermetic on disk regardless of premature exit.
//
// We pass the resulting fd to mmap via _open_osfhandle equivalents — except
// the libc's mmap on Windows actually walks the fd table for file-backed
// MAP_PRIVATE, so we install the handle as a libc fd via the shared fd
// install path (open_osfhandle is the public name).
// ---------------------------------------------------------------------------

#include "src/__support/OSUtil/windows/temp_path.h"
#include "src/unistd/_open_osfhandle.h"
#include "src/unistd/close.h"

namespace {

// Build a unique \??\<temp>\llvm_libc_priv_<tag>_<tid>.bin path. The tag
// argument keeps phase files distinct under parallel test execution.
struct TempFilePath {
  // Wide buffer big enough for "\??\" + temp dir + filename + NUL.
  WCHAR buf[512];
  USHORT byte_len; // length excluding NUL, in bytes
};

LIBC_INLINE void append_w(TempFilePath &p, size_t &cursor, const WCHAR *s) {
  while (*s != u'\0' && cursor + 1 < sizeof(p.buf) / sizeof(WCHAR))
    p.buf[cursor++] = *s++;
}

LIBC_INLINE void append_hex(TempFilePath &p, size_t &cursor, uint32_t v) {
  static constexpr WCHAR kHex[] = u"0123456789abcdef";
  for (int shift = 28; shift >= 0; shift -= 4) {
    if (cursor + 1 >= sizeof(p.buf) / sizeof(WCHAR))
      return;
    p.buf[cursor++] = kHex[(v >> shift) & 0xF];
  }
}

TempFilePath make_temp_path(const WCHAR *tag) {
  TempFilePath p{};
  size_t cursor = 0;

  // NT-style prefix so OBJECT_ATTRIBUTES routes through ObpDosDevices.
  static constexpr WCHAR kNtPrefix[] = u"\\??\\";
  append_w(p, cursor, kNtPrefix);

  // Resolve the temp directory; get_temp_path_w writes a DOS-form path with
  // a trailing backslash. Strip a leading "X:\" — no, keep it as-is: the
  // \??\ prefix expects a DOS path immediately after.
  WCHAR temp_dos[260];
  size_t temp_len =
      LIBC_NAMESPACE::windows::get_temp_path_w(temp_dos, 260);
  if (temp_len == 0) {
    // Fallback to a hard-coded location; tests run as a normal user but the
    // Windows install always has a writable C:\Windows\Temp.
    static constexpr WCHAR kFallback[] = u"C:\\Windows\\Temp\\";
    for (size_t i = 0; kFallback[i] != u'\0'; ++i)
      p.buf[cursor++] = kFallback[i];
  } else {
    for (size_t i = 0; i < temp_len; ++i)
      p.buf[cursor++] = temp_dos[i];
  }

  static constexpr WCHAR kStem[] = u"llvm_libc_priv_";
  append_w(p, cursor, kStem);
  append_w(p, cursor, tag);
  p.buf[cursor++] = u'_';

  // Disambiguator: TID + a monotonic counter so concurrent threads never
  // collide on the same path even within one phase.
  static Atomic<uint32_t> seq{0};
  append_hex(p, cursor, ::NtCurrentThreadId());
  p.buf[cursor++] = u'_';
  append_hex(p, cursor, seq.fetch_add(1, MemoryOrder::RELAXED));

  static constexpr WCHAR kExt[] = u".bin";
  append_w(p, cursor, kExt);

  p.buf[cursor] = u'\0';
  p.byte_len = static_cast<USHORT>(cursor * sizeof(WCHAR));
  return p;
}

// Open a file for read/write. FILE_DELETE_ON_CLOSE keeps the disk clean even
// on test crashes — a stray close still removes the artifact. SHARE_DELETE
// lets a second handle (the verification handle) be opened concurrently.
HANDLE create_file(const TempFilePath &path, ACCESS_MASK access,
                   ULONG disposition) {
  UNICODE_STRING us;
  us.Buffer = reinterpret_cast<PWSTR>(const_cast<WCHAR *>(path.buf));
  us.Length = path.byte_len;
  us.MaximumLength = path.byte_len + sizeof(WCHAR);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  HANDLE h = nullptr;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS s = ::NtCreateFile(
      &h, access | SYNCHRONIZE | DELETE_ACCESS, &oa, &iosb, nullptr,
      FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  return NT_SUCCESS(s) ? h : nullptr;
}

// Synchronous read at an absolute offset. Returns bytes read on success,
// negative on failure. Used for the verification handle.
LONG read_at(HANDLE h, ULONGLONG offset, void *buf, ULONG len) {
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER off;
  off.QuadPart = static_cast<LONGLONG>(offset);
  NTSTATUS s = ::NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buf, len,
                            &off, nullptr);
  if (!NT_SUCCESS(s))
    return -1;
  return static_cast<LONG>(iosb.Information);
}

LONG write_at(HANDLE h, ULONGLONG offset, const void *buf, ULONG len) {
  IO_STATUS_BLOCK iosb = {};
  LARGE_INTEGER off;
  off.QuadPart = static_cast<LONGLONG>(offset);
  NTSTATUS s = ::NtWriteFile(h, nullptr, nullptr, nullptr, &iosb,
                             const_cast<void *>(buf), len, &off, nullptr);
  if (!NT_SUCCESS(s))
    return -1;
  return static_cast<LONG>(iosb.Information);
}

// Fill the file with a deterministic pattern, 4-byte words. The pattern is
// uniform per page so any single-word check at a page offset suffices to
// witness page-level integrity; the per-page distinct seed catches stray
// page-shuffles that a uniform fill would silently accept.
void fill_pattern(HANDLE h, size_t total_bytes) {
  alignas(64) ULONG page_buf[kAllocGranularity / sizeof(ULONG)];
  size_t pages = total_bytes / kAllocGranularity;
  for (size_t pg = 0; pg < pages; ++pg) {
    ULONG seed = kPagePattern ^ static_cast<ULONG>(pg * 0x9E37'79B9u);
    for (size_t i = 0; i < kAllocGranularity / sizeof(ULONG); ++i)
      page_buf[i] = seed ^ static_cast<ULONG>(i);
    LONG w = write_at(h, pg * kAllocGranularity, page_buf, kAllocGranularity);
    (void)w;
  }
}

// Compute the original file word that should appear at (page_index, word_index)
// after fill_pattern.
LIBC_INLINE ULONG original_word(size_t page_index, size_t word_index) {
  ULONG seed = kPagePattern ^ static_cast<ULONG>(page_index * 0x9E37'79B9u);
  return seed ^ static_cast<ULONG>(word_index);
}

// Install an NT HANDLE into the libc fd table so mmap() can reach it. We then
// own the fd; close() releases both the fd and the underlying handle. On any
// failure path we close the raw HANDLE ourselves.
int install_as_fd(HANDLE h) {
  int fd = LIBC_NAMESPACE::_open_osfhandle(reinterpret_cast<intptr_t>(h), 0);
  return fd;
}

// Read the file's current bytes via a fresh, independent handle that does not
// pass through the mapping. This is the witness for "did CoW leak to disk?".
bool verify_file_unchanged(const TempFilePath &path, size_t total_bytes) {
  HANDLE h = create_file(path, FILE_GENERIC_READ, FILE_OPEN);
  if (!h)
    return false;
  bool ok = true;
  size_t pages = total_bytes / kAllocGranularity;
  alignas(64) ULONG buf[kAllocGranularity / sizeof(ULONG)];
  for (size_t pg = 0; ok && pg < pages; ++pg) {
    LONG r = read_at(h, pg * kAllocGranularity, buf, kAllocGranularity);
    if (r != static_cast<LONG>(kAllocGranularity)) {
      ok = false;
      break;
    }
    for (size_t i = 0; i < kAllocGranularity / sizeof(ULONG); ++i) {
      if (buf[i] != original_word(pg, i)) {
        ok = false;
        break;
      }
    }
  }
  ::NtClose(h);
  return ok;
}

// Best-effort working-set probe via NtQueryInformationProcess(VmCounters).
// PagefileUsage is the process private commit charge — the metric we care
// about when looking for accumulated leaks in the placeholder/region pool.
SIZE_T process_private_commit() {
  VM_COUNTERS vmc = {};
  NTSTATUS s =
      ::NtQueryInformationProcess(NtCurrentProcess(), ProcessVmCounters, &vmc,
                                  sizeof(vmc), nullptr);
  if (!NT_SUCCESS(s))
    return 0;
  return vmc.PagefileUsage;
}

// RAII wrapper for (path, libc-fd) pair. Closing the fd closes the underlying
// handle which (because of FILE_DELETE_ON_CLOSE) unlinks the file. Move-only
// to make leak/order errors a compile-time problem.
struct TempFile {
  TempFilePath path{};
  int fd = -1;
  size_t size = 0;

  TempFile() = default;
  TempFile(TempFile &&o) noexcept : path(o.path), fd(o.fd), size(o.size) {
    o.fd = -1;
  }
  TempFile &operator=(TempFile &&o) noexcept {
    if (this != &o) {
      close();
      path = o.path;
      fd = o.fd;
      size = o.size;
      o.fd = -1;
    }
    return *this;
  }
  ~TempFile() { close(); }

  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;

  void close() {
    if (fd >= 0) {
      LIBC_NAMESPACE::close(fd);
      fd = -1;
    }
  }
};

// Create a CoW-friendly temp file: pages-of-pattern, sized to a multiple of
// 64K. Returns an empty TempFile on failure (caller asserts).
TempFile make_filled_temp_file(const WCHAR *tag, size_t pages) {
  TempFile out;
  out.path = make_temp_path(tag);
  out.size = pages * kAllocGranularity;

  HANDLE h = create_file(out.path,
                         FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                         FILE_OVERWRITE_IF);
  if (!h)
    return out; // fd stays -1; caller will fail
  fill_pattern(h, out.size);
  out.fd = install_as_fd(h);
  if (out.fd < 0) {
    ::NtClose(h);
    return TempFile{};
  }
  return out;
}

// Convenience: read a 32-bit word at a page offset of the mapping.
LIBC_INLINE ULONG load_word(volatile void *base, size_t page, size_t word) {
  auto *p = reinterpret_cast<volatile ULONG *>(base);
  return p[(page * kAllocGranularity) / sizeof(ULONG) + word];
}

LIBC_INLINE void store_word(volatile void *base, size_t page, size_t word,
                            ULONG value) {
  auto *p = reinterpret_cast<volatile ULONG *>(base);
  p[(page * kAllocGranularity) / sizeof(ULONG) + word] = value;
}

// Dirty every page in `[base, base+pages*64K)` with a thread/iter-tagged
// CoW pattern. We write at word index 0 and at the last word of the page so
// that subsequent partial-unmaps that decommit either edge are caught.
void dirty_cow_range(void *base, size_t pages, ULONG tag) {
  for (size_t pg = 0; pg < pages; ++pg) {
    store_word(base, pg, 0, tag ^ static_cast<ULONG>(pg));
    store_word(base, pg, (kAllocGranularity / sizeof(ULONG)) - 1,
               ~(tag ^ static_cast<ULONG>(pg)));
  }
}

bool verify_cow_range(void *base, size_t pages, ULONG tag) {
  for (size_t pg = 0; pg < pages; ++pg) {
    ULONG expect_first = tag ^ static_cast<ULONG>(pg);
    ULONG expect_last = ~expect_first;
    if (load_word(base, pg, 0) != expect_first)
      return false;
    if (load_word(base, pg, (kAllocGranularity / sizeof(ULONG)) - 1) !=
        expect_last)
      return false;
  }
  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Phase 1 — basic CoW sanity. No shape promotion. Confirms:
//   * mmap(MAP_PRIVATE) on a real file returns readable pages with the
//     original file content.
//   * Stores into the mapping are PAGE_WRITECOPY: the section view diverges
//     locally, but the backing file (read via an independent handle) does
//     not change. If the engine accidentally created a MAP_SHARED view —
//     or stamped REGION_FLAG_COW wrong — we'd see the writes leak to disk.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapPrivateFileStressTest, Phase1_BasicCowSanity) {
  constexpr size_t PAGES = 4;
  TempFile tf = make_filled_temp_file(u"p1", PAGES);
  ASSERT_GE(tf.fd, 0);

  void *m = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(m, MAP_FAILED);

  // Original pattern visible.
  for (size_t pg = 0; pg < PAGES; ++pg)
    EXPECT_EQ(load_word(m, pg, 0), original_word(pg, 0));

  // CoW: stomp every page.
  dirty_cow_range(m, PAGES, 0xCA'FE'00'01u);

  // Local view sees the dirty data.
  EXPECT_TRUE(verify_cow_range(m, PAGES, 0xCA'FE'00'01u));

  // Backing file was NOT modified — this is the load-bearing CoW invariant.
  EXPECT_TRUE(verify_file_unchanged(tf.path, tf.size));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(m, tf.size), Succeeds());
}

// ---------------------------------------------------------------------------
// Phase 2 — partial unmap of a fully-dirtied CoW region forces the
// FILE_VIEW_MONO → FILE_VIEW_CHUNKED shape promotion. The promotion path
// must call CowContext::save_cow_pages around the section-view teardown
// because NT's NtUnmapViewOfSection drops the writecopy state — without
// the save/restore, the surviving head/tail fragments would re-read the
// original file content.
//
// This is the exact bug class the redesign closes via REGION_FLAG_COW
// driving the CowContext lifecycle in vm_protect.cpp::partial_unmap_view.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapPrivateFileStressTest,
       Phase2_PartialUnmapPreservesCoWPages) {
  constexpr size_t PAGES = 8;
  TempFile tf = make_filled_temp_file(u"p2", PAGES);
  ASSERT_GE(tf.fd, 0);

  void *m = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(m, MAP_FAILED);

  ULONG tag = 0xDE'AD'00'02u;
  dirty_cow_range(m, PAGES, tag);

  // Punch the middle two 64K chunks. After this call the region must be
  // shape=CHUNKED with chunks {[0..3), [5..8)}; head and tail must still
  // observe the dirty CoW pattern.
  char *base = static_cast<char *>(m);
  void *hole = base + 3 * kAllocGranularity;
  EXPECT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kAllocGranularity),
              Succeeds());

  // Head [0..3) — CoW data preserved.
  for (size_t pg = 0; pg < 3; ++pg) {
    ULONG ef = tag ^ static_cast<ULONG>(pg);
    EXPECT_EQ(load_word(base, pg, 0), ef);
    EXPECT_EQ(load_word(base, pg, (kAllocGranularity / sizeof(ULONG)) - 1),
              ~ef);
  }
  // Tail [5..8) — CoW data preserved.
  for (size_t pg = 5; pg < PAGES; ++pg) {
    ULONG ef = tag ^ static_cast<ULONG>(pg);
    EXPECT_EQ(load_word(base, pg, 0), ef);
    EXPECT_EQ(load_word(base, pg, (kAllocGranularity / sizeof(ULONG)) - 1),
              ~ef);
  }

  // Backing file remains the original pattern even though the surviving
  // fragments hold dirty data.
  EXPECT_TRUE(verify_file_unchanged(tf.path, tf.size));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kAllocGranularity),
              Succeeds());
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 5 * kAllocGranularity,
                                     3 * kAllocGranularity),
              Succeeds());
}

// ---------------------------------------------------------------------------
// Phase 3 — concurrent partial-unmap of one CoW region. Pins:
//
//   (a) the MONO→CHUNKED transition is exclusive: only one thread wins;
//       losers wait on the shape word (ThreadLocalWord::wait_for_addr in
//       region_pool.cpp) and re-dispatch into the CHUNKED recipe;
//   (b) once CHUNKED, simultaneous chunk-list mutations (further punches)
//       are serialized by the chunk_list parking mutex without losing CoW
//       state on adjacent surviving chunks;
//   (c) `commit_remap` rollback (the new `register_or_rollback_right` path)
//       leaves the surviving fragments registered if any single sub-op
//       fails — verified indirectly by the surviving-pages read-back
//       succeeding for every thread's slice.
// ---------------------------------------------------------------------------

namespace {

struct Phase3Ctx {
  void *base;
  size_t pages;          // total pages in the mapping
  size_t slice_pages;    // pages per thread slice
  Atomic<int> errors{0};
  Atomic<int> ready{0};
  Atomic<int> go{0};
};

DWORD phase3_worker(void *arg) {
  auto *ctx = static_cast<Phase3Ctx *>(arg);
  DWORD tid = ::NtCurrentThreadId();
  // Slice index from the thread's position; we hash TID into a small index
  // by claiming a sequential slot via `ready` count.
  int slot = ctx->ready.fetch_add(1, MemoryOrder::ACQ_REL);
  size_t slice_start = static_cast<size_t>(slot) * ctx->slice_pages;

  // Wait at the start gate so all threads punch concurrently. Bounded so
  // a parent death pre-gate converts a worker hang into a deterministic
  // exit with the errors counter incremented.
  {
    int waited_ms = 0;
    while (ctx->go.load(MemoryOrder::ACQUIRE) == 0 && waited_ms < 5000) {
      LIBC_NAMESPACE::test_support::sleep_ms(1);
      ++waited_ms;
    }
    if (ctx->go.load(MemoryOrder::ACQUIRE) == 0) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      return 0;
    }
  }

  ULONG tag = 0x9000'0000u | (tid & 0x0FFF'FFFFu);
  char *base = static_cast<char *>(ctx->base);
  void *slice_base = base + slice_start * kAllocGranularity;

  // Step 1: dirty every page in this slice. The slices are disjoint so
  // multiple threads can store concurrently without interfering.
  dirty_cow_range(slice_base, ctx->slice_pages, tag);

  // Step 2: punch a single 64K hole at slice-relative position `slot % slice`.
  // Different threads pick different in-slice positions, so the resulting
  // chunk count grows as the punches land. The hole is never at slice edge,
  // ensuring head+tail of the slice both survive as separate chunks.
  size_t hole_off = 1 + (static_cast<size_t>(slot) % (ctx->slice_pages - 2));
  void *hole = static_cast<char *>(slice_base) + hole_off * kAllocGranularity;
  if (LIBC_NAMESPACE::munmap(hole, kAllocGranularity) != 0) {
    ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    return 0;
  }

  // Step 3: read back every surviving page in the slice. Any CoW data lost
  // to the promotion or to a concurrent commit_remap rollback would surface
  // as a stale word here.
  for (size_t pg = 0; pg < ctx->slice_pages; ++pg) {
    if (pg == hole_off)
      continue;
    ULONG ef = tag ^ static_cast<ULONG>(pg);
    if (load_word(slice_base, pg, 0) != ef)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    if (load_word(slice_base, pg,
                  (kAllocGranularity / sizeof(ULONG)) - 1) != ~ef)
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

} // anonymous namespace

TEST_F(LlvmLibcMmapPrivateFileStressTest, Phase3_ConcurrentPartialUnmapsCoW) {
  constexpr int N = 4;
  constexpr size_t SLICE_PAGES = 4;
  constexpr size_t TOTAL_PAGES = N * SLICE_PAGES;

  TempFile tf = make_filled_temp_file(u"p3", TOTAL_PAGES);
  ASSERT_GE(tf.fd, 0);

  void *m = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(m, MAP_FAILED);

  Phase3Ctx ctx{};
  ctx.base = m;
  ctx.pages = TOTAL_PAGES;
  ctx.slice_pages = SLICE_PAGES;

  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(phase3_worker, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  // Wait for every worker to claim its slice, then release the gate so the
  // partial-munmap calls overlap in time. Bounded so a worker dying before
  // it claims its slice becomes a deterministic test failure rather than
  // an indefinite parent hang.
  {
    int waited_ms = 0;
    while (ctx.ready.load(MemoryOrder::ACQUIRE) < N && waited_ms < 5000) {
      LIBC_NAMESPACE::test_support::sleep_ms(1);
      ++waited_ms;
    }
    ASSERT_EQ(ctx.ready.load(MemoryOrder::ACQUIRE), N);
  }
  ctx.go.store(1, MemoryOrder::RELEASE);

  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    60000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);

  // Backing file content is the final witness — none of the per-thread CoW
  // patterns may have leaked to disk.
  EXPECT_TRUE(verify_file_unchanged(tf.path, tf.size));

  // Drain whatever survives. We know each thread punched one page, so the
  // surviving footprint is total - N pages, but the layout is decided by
  // the in-slice hole positions. Brute-force munmap each 64K unit; munmap
  // of an already-freed range is a no-op (libc returns 0 for "no overlap").
  char *base = static_cast<char *>(m);
  for (size_t pg = 0; pg < TOTAL_PAGES; ++pg) {
    LIBC_NAMESPACE::munmap(base + pg * kAllocGranularity, kAllocGranularity);
  }
}

// ---------------------------------------------------------------------------
// Phase 4 — promote then fully unmap. After MONO→CHUNKED, peeling each
// remaining chunk in turn must:
//   * decrement the RegionDesc refcount per chunk;
//   * release the section handle exactly once when refcount hits zero
//     (validated indirectly by re-mmap of the same path succeeding and
//     observing the pristine file content).
// A leak in the per-chunk refcount accounting would either (a) keep the
// section alive past its last view, eventually exhausting handles under
// repeated runs, or (b) double-close the section, corrupting unrelated
// region state.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapPrivateFileStressTest, Phase4_PromoteThenFullyUnmap) {
  constexpr size_t PAGES = 8;
  TempFile tf = make_filled_temp_file(u"p4", PAGES);
  ASSERT_GE(tf.fd, 0);

  void *m = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(m, MAP_FAILED);

  ULONG tag = 0xBE'EF'00'04u;
  dirty_cow_range(m, PAGES, tag);

  char *base = static_cast<char *>(m);

  // Promote: middle 2 chunks gone. Region is now CHUNKED with two chunks.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 3 * kAllocGranularity,
                                     2 * kAllocGranularity),
              Succeeds());

  // Drop the head chunk first.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kAllocGranularity),
              Succeeds());

  // Tail chunk still readable, still CoW-dirty. We check at absolute page
  // indices (5..7) so the original `tag ^ pg` invariant holds verbatim —
  // verify_cow_range starts at page 0 of its base, which would re-derive the
  // wrong expected words for the relocated pointer.
  for (size_t pg = 5; pg < PAGES; ++pg) {
    ULONG ef = tag ^ static_cast<ULONG>(pg);
    EXPECT_EQ(load_word(base, pg, 0), ef);
    EXPECT_EQ(load_word(base, pg, (kAllocGranularity / sizeof(ULONG)) - 1),
              ~ef);
  }

  // Drop the tail chunk; region refcount must hit zero now.
  EXPECT_THAT(LIBC_NAMESPACE::munmap(base + 5 * kAllocGranularity,
                                     3 * kAllocGranularity),
              Succeeds());

  // Re-map the same file path. If the previous teardown leaked the section
  // handle the OFD cache or the file system might return a stale state; we
  // observe the pristine pattern.
  HANDLE h2 = create_file(tf.path,
                          FILE_GENERIC_READ | FILE_GENERIC_WRITE, FILE_OPEN);
  ASSERT_NE(h2, static_cast<HANDLE>(nullptr));
  int fd2 = install_as_fd(h2);
  ASSERT_GE(fd2, 0);

  void *m2 = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE, fd2, 0);
  ASSERT_NE(m2, MAP_FAILED);
  for (size_t pg = 0; pg < PAGES; ++pg)
    EXPECT_EQ(load_word(m2, pg, 0), original_word(pg, 0));

  EXPECT_THAT(LIBC_NAMESPACE::munmap(m2, tf.size), Succeeds());
  LIBC_NAMESPACE::close(fd2);

  EXPECT_TRUE(verify_file_unchanged(tf.path, tf.size));
}

// ---------------------------------------------------------------------------
// Phase 5 — long-running mixed-thread churn. Each worker repeatedly:
//   1. creates a temp file;
//   2. mmap MAP_PRIVATE;
//   3. writes a CoW pattern;
//   4. partial-unmaps a hole (driving MONO→CHUNKED + CowContext);
//   5. drains every remaining chunk;
//   6. closes and unlinks (FILE_DELETE_ON_CLOSE handles unlink).
//
// After the churn we measure process private commit and assert it has not
// grown beyond a generous bound (4× baseline). A leak in the placeholder
// VA, the region pool slot freelist, or the section handle refcount would
// surface here as a monotonically growing private commit.
// ---------------------------------------------------------------------------

namespace {

struct Phase5Ctx {
  Atomic<int> errors{0};
  Atomic<int> done_iters{0};
  int target_iters_per_thread;
};

DWORD phase5_worker(void *arg) {
  auto *ctx = static_cast<Phase5Ctx *>(arg);
  DWORD tid = ::NtCurrentThreadId();
  for (int iter = 0; iter < ctx->target_iters_per_thread; ++iter) {
    constexpr size_t PAGES = 4;
    TempFile tf = make_filled_temp_file(u"p5", PAGES);
    if (tf.fd < 0) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }

    void *m = LIBC_NAMESPACE::mmap(nullptr, tf.size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE, tf.fd, 0);
    if (m == MAP_FAILED) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
      continue;
    }

    ULONG tag = (static_cast<ULONG>(tid) << 8) ^ static_cast<ULONG>(iter);
    dirty_cow_range(m, PAGES, tag);

    char *base = static_cast<char *>(m);

    // Punch a single page; promotes MONO → CHUNKED.
    if (LIBC_NAMESPACE::munmap(base + 1 * kAllocGranularity,
                               kAllocGranularity) != 0) {
      ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    }

    // Verify surviving chunks still hold CoW data.
    for (size_t pg : {size_t(0), size_t(2), size_t(3)}) {
      ULONG ef = tag ^ static_cast<ULONG>(pg);
      if (load_word(base, pg, 0) != ef)
        ctx->errors.fetch_add(1, MemoryOrder::RELAXED);
    }

    // Drain the remaining two chunks in any order.
    LIBC_NAMESPACE::munmap(base, kAllocGranularity);
    LIBC_NAMESPACE::munmap(base + 2 * kAllocGranularity,
                           2 * kAllocGranularity);

    // close()→NtClose→FILE_DELETE_ON_CLOSE removes the temp file.
    ctx->done_iters.fetch_add(1, MemoryOrder::RELAXED);
  }
  return 0;
}

} // anonymous namespace

TEST_F(LlvmLibcMmapPrivateFileStressTest, Phase5_StressMixedThreads) {
  constexpr int N = 6;
  constexpr int K = 50;

  // Warm the allocator so the baseline reading isn't biased by first-touch
  // inflation of the region pool / placeholder reservations.
  {
    TempFile warm = make_filled_temp_file(u"p5w", 4);
    ASSERT_GE(warm.fd, 0);
    void *m = LIBC_NAMESPACE::mmap(nullptr, warm.size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE, warm.fd, 0);
    ASSERT_NE(m, MAP_FAILED);
    LIBC_NAMESPACE::munmap(m, warm.size);
  }

  SIZE_T baseline = process_private_commit();

  Phase5Ctx ctx{};
  ctx.target_iters_per_thread = K;

  HANDLE ths[N];
  for (int i = 0; i < N; ++i) {
    ths[i] = LIBC_NAMESPACE::test_support::create_thread(phase5_worker, &ctx);
    ASSERT_NE(ths[i], static_cast<HANDLE>(nullptr));
  }
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(LIBC_NAMESPACE::test_support::wait_for_single_object(ths[i],
                                                                    120000),
              static_cast<DWORD>(LIBC_NAMESPACE::test_support::WAIT_OBJECT_0));
    ::NtClose(ths[i]);
  }

  EXPECT_EQ(ctx.errors.load(MemoryOrder::RELAXED), 0);
  EXPECT_EQ(ctx.done_iters.load(MemoryOrder::RELAXED), N * K);

  // Generous leak bound: 4× baseline OR baseline + 64 MiB, whichever is
  // larger. Anything beyond that is a real accumulation bug, not measurement
  // noise. The slack accommodates legitimate growth in the region pool's
  // internal freelist as it commits more chunk pages on first use.
  SIZE_T after = process_private_commit();
  if (baseline > 0) {
    SIZE_T cap_mul = baseline * 4;
    SIZE_T cap_abs = baseline + (64ull * 1024 * 1024);
    SIZE_T cap = cap_mul > cap_abs ? cap_mul : cap_abs;
    EXPECT_LE(after, cap);
  }
}
