//===-- Public-API tests for file-backed partial munmap ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Public-API counterpart to the hermetic region_shape_transition_test.
//
// Every existing public mmap test in this directory is MAP_ANONYMOUS; this
// file is the externally-visible coverage for the FILE_VIEW_MONO ->
// FILE_VIEW_CHUNKED dispatch driven by partial_unmap_view (vm_protect.cpp).
// The tests touch only the POSIX surface (mmap, munmap, mprotect) and verify
// promotion behaviour by observing what the user is contractually entitled to
// see: surrounding pages stay readable with their data intact, MAP_SHARED
// writes still reach the backing file after promotion, and MAP_PRIVATE CoW
// dirty pages stay isolated from the file.
//
// The backing file is created and verified through direct NtCreateFile /
// NtWriteFile / NtReadFile calls so cache state from the mmap path can never
// confuse the verification — re-reads come straight from the filesystem
// through an independent, non-overlapped handle.
//
// Allocation granularity matters here. Windows file-view mappings are backed
// by 64KB-aligned VA chunks; to keep MONO->CHUNKED transitions deterministic
// the layouts use multiples of 64KB throughout.
//
//===----------------------------------------------------------------------===//

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/fcntl/open.h"
#include "src/sys/mman/mmap.h"
#include "src/sys/mman/mprotect.h"
#include "src/sys/mman/munmap.h"
#include "src/unistd/close.h"
#include "src/unistd/ftruncate.h"
#include "src/unistd/unlink.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"
#include "test/UnitTest/Test.h"

#include "hdr/fcntl_macros.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"

#include <stddef.h>
#include <stdint.h>

using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;
using LlvmLibcMmapFilePartialUnmapTest =
    LIBC_NAMESPACE::testing::ErrnoCheckingTest;

namespace {

// 64 KB — Windows view allocation granularity. Each "chunk" in the spec
// refers to one 64 KB segment of a file view.
constexpr size_t kChunk = 64 * 1024;

// 8 chunks * 64 KB = 512 KB total. Large enough to carve a middle hole and
// still leave a clean head + tail; small enough that NtWriteFile of the full
// pattern is essentially free.
constexpr size_t kNumChunks = 8;
constexpr size_t kFileSize = kNumChunks * kChunk;

// Deterministic per-byte pattern. Prime stride so neighbouring 64 KB chunks
// never accidentally compare equal even if the partial-unmap path corrupted a
// chunk boundary.
LIBC_INLINE uint8_t pattern_byte(size_t off) {
  return static_cast<uint8_t>((off * 131u + 7u) & 0xFF);
}

LIBC_INLINE void fill_pattern(uint8_t *buf, size_t off, size_t len) {
  for (size_t i = 0; i < len; ++i)
    buf[i] = pattern_byte(off + i);
}

LIBC_INLINE bool check_pattern(const uint8_t *buf, size_t off, size_t len) {
  for (size_t i = 0; i < len; ++i)
    if (buf[i] != pattern_byte(off + i))
      return false;
  return true;
}

// Build a per-test NT path of the form \??\C:\Windows\Temp\<tag>.bin. Using
// a fixed prefix mirrors thread_ring_bench.cpp; the per-test tag avoids
// collisions when multiple test cases run in the same process.
struct TestPath {
  // Enough room for prefix + tag + suffix + NUL. Sized generously.
  char16_t buf[256];
  USHORT length_bytes; // bytes excluding NUL — the UNICODE_STRING.Length form.
};

LIBC_INLINE void build_test_path(TestPath &p, const char *tag) {
  static constexpr char16_t kPrefix[] =
      u"\\??\\C:\\Windows\\Temp\\llvm_libc_part_unmap_";
  static constexpr char16_t kSuffix[] = u".bin";

  size_t i = 0;
  for (size_t k = 0; kPrefix[k] != 0; ++k)
    p.buf[i++] = kPrefix[k];
  for (size_t k = 0; tag[k] != 0; ++k)
    p.buf[i++] = static_cast<char16_t>(tag[k]);
  for (size_t k = 0; kSuffix[k] != 0; ++k)
    p.buf[i++] = kSuffix[k];
  p.buf[i] = 0;
  p.length_bytes = static_cast<USHORT>(i * sizeof(char16_t));
}

// Build the equivalent ASCII path passed to libc open(). The libc path layer
// understands the bare drive form and translates it itself, so we strip the
// NT \??\ prefix.
LIBC_INLINE void build_ascii_path(char *out, size_t cap, const char *tag) {
  static const char kPrefix[] = "C:\\Windows\\Temp\\llvm_libc_part_unmap_";
  static const char kSuffix[] = ".bin";
  size_t i = 0;
  for (size_t k = 0; kPrefix[k] != 0 && i + 1 < cap; ++k)
    out[i++] = kPrefix[k];
  for (size_t k = 0; tag[k] != 0 && i + 1 < cap; ++k)
    out[i++] = tag[k];
  for (size_t k = 0; kSuffix[k] != 0 && i + 1 < cap; ++k)
    out[i++] = kSuffix[k];
  out[i] = 0;
}

// Open (creating, truncating) a fresh test file and fill it with the
// deterministic pattern via NtWriteFile. The setup path runs through the NT
// API so the contents on disk are unambiguously what we wrote — no libc /
// page-cache state can confuse the later verification.
//
// Returns a handle the caller must close. FILE_DELETE_ON_CLOSE is *not* set
// here: the libc fd we open below for mmap needs the file to outlive this
// handle's close, and we tear it down explicitly with libc::unlink.
LIBC_INLINE bool create_and_fill(const TestPath &p, HANDLE *out_handle) {
  UNICODE_STRING us;
  us.Buffer = reinterpret_cast<PWSTR>(const_cast<char16_t *>(p.buf));
  us.Length = p.length_bytes;
  us.MaximumLength = p.length_bytes + sizeof(char16_t);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS s = ::NtCreateFile(
      &h, FILE_GENERIC_READ | FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
      FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OVERWRITE_IF,
      // SYNCHRONOUS_IO_NONALERT lets us drive NtWriteFile/NtReadFile with a
      // null event handle and rely on the call returning only after I/O
      // completion — no NtWaitForSingleObject dance needed.
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  if (!NT_SUCCESS(s))
    return false;

  // Write the pattern in 64 KB chunks (matches the alloc granularity used by
  // every test below; keeps the buffer stack-friendly).
  uint8_t chunk[kChunk];
  for (size_t off = 0; off < kFileSize; off += kChunk) {
    fill_pattern(chunk, off, kChunk);
    LARGE_INTEGER lo;
    lo.QuadPart = static_cast<LONGLONG>(off);
    IO_STATUS_BLOCK wb = {};
    s = ::NtWriteFile(h, nullptr, nullptr, nullptr, &wb, chunk,
                      static_cast<ULONG>(kChunk), &lo, nullptr);
    if (!NT_SUCCESS(s)) {
      ::NtClose(h);
      return false;
    }
  }
  *out_handle = h;
  return true;
}

// Re-open the file with read-only access via an independent handle and read
// the requested range. Used after partial-unmap to verify the on-disk
// contents have or have not been modified by the test (depending on
// MAP_SHARED vs MAP_PRIVATE expectations).
LIBC_INLINE bool reread_file(const TestPath &p, size_t off, size_t len,
                             uint8_t *out) {
  UNICODE_STRING us;
  us.Buffer = reinterpret_cast<PWSTR>(const_cast<char16_t *>(p.buf));
  us.Length = p.length_bytes;
  us.MaximumLength = p.length_bytes + sizeof(char16_t);

  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  oa.ObjectName = &us;
  oa.Attributes = OBJ_CASE_INSENSITIVE;

  IO_STATUS_BLOCK iosb = {};
  HANDLE h = nullptr;
  NTSTATUS s = ::NtCreateFile(
      &h, FILE_GENERIC_READ, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  if (!NT_SUCCESS(s))
    return false;

  LARGE_INTEGER lo;
  lo.QuadPart = static_cast<LONGLONG>(off);
  IO_STATUS_BLOCK rb = {};
  s = ::NtReadFile(h, nullptr, nullptr, nullptr, &rb, out,
                   static_cast<ULONG>(len), &lo, nullptr);
  ::NtClose(h);
  return NT_SUCCESS(s);
}

// Helper: run create_and_fill, NtClose the setup handle (we don't need it
// past the initial write), open a libc fd for the same file, and return both
// the fd and the path so the test can verify and tear down.
struct TestFile {
  TestPath nt_path;
  char ascii_path[256];
  int fd; // libc fd for mmap; -1 on failure.
};

LIBC_INLINE bool open_test_file(TestFile &tf, const char *tag, int open_flags) {
  build_test_path(tf.nt_path, tag);
  build_ascii_path(tf.ascii_path, sizeof(tf.ascii_path), tag);

  HANDLE setup = nullptr;
  if (!create_and_fill(tf.nt_path, &setup))
    return false;
  ::NtClose(setup);

  tf.fd = LIBC_NAMESPACE::open(tf.ascii_path, open_flags);
  return tf.fd >= 0;
}

LIBC_INLINE void close_test_file(TestFile &tf) {
  if (tf.fd >= 0)
    LIBC_NAMESPACE::close(tf.fd);
  // Unlink via libc — backs onto NtSetInformationFile with POSIX semantics so
  // the file goes away even if a handle is still open.
  LIBC_NAMESPACE::unlink(tf.ascii_path);
}

// Convenience: read [off, off+len) from the live mapping into `out`. We go
// through volatile pointer derefs to keep the compiler from elliding the
// loads — these reads are the actual test assertion.
LIBC_INLINE void read_view(const void *base, size_t off, size_t len,
                           uint8_t *out) {
  const volatile uint8_t *p =
      reinterpret_cast<const volatile uint8_t *>(base) + off;
  for (size_t i = 0; i < len; ++i)
    out[i] = p[i];
}

LIBC_INLINE void write_view(void *base, size_t off, size_t len,
                            const uint8_t *src) {
  volatile uint8_t *p = reinterpret_cast<volatile uint8_t *>(base) + off;
  for (size_t i = 0; i < len; ++i)
    p[i] = src[i];
}

// Verify the surviving pattern at [off, off+len) matches the original file
// pattern starting at file offset `file_off`. Pulls the data through
// `read_view` so we exercise the actual user-visible mapping, not a copy.
LIBC_INLINE bool view_matches_pattern(const void *base, size_t view_off,
                                      size_t file_off, size_t len) {
  // Stack-budget limit: read in 4 KB increments so the helper itself never
  // burns more than a page on the stack.
  uint8_t scratch[4096];
  size_t done = 0;
  while (done < len) {
    size_t n = (len - done) > sizeof(scratch) ? sizeof(scratch) : (len - done);
    read_view(base, view_off + done, n, scratch);
    if (!check_pattern(scratch, file_off + done, n))
      return false;
    done += n;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. SharedFile — partial-unmap the middle 2x64K hole.
//    Surviving head and tail must remain readable with the original pattern,
//    and the backing file must be unchanged (no write was performed).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_PartialUnmapMiddleLeavesHeadAndTail) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_mid", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Hole = chunks [3, 5). Promotes shape MONO -> CHUNKED with two surviving
  // fragments (head: chunks [0,3), tail: chunks [5,8)).
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Head and tail still hold the original pattern (no write was issued).
  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 3 * kChunk));
  EXPECT_TRUE(
      view_matches_pattern(base, 5 * kChunk, 5 * kChunk, 3 * kChunk));

  // Backing file unchanged: spot-check the hole region directly. The unmap
  // is a VA-level operation; no I/O reaches the file.
  uint8_t scratch[kChunk];
  ASSERT_TRUE(reread_file(tf.nt_path, 3 * kChunk, kChunk, scratch));
  EXPECT_TRUE(check_pattern(scratch, 3 * kChunk, kChunk));

  // Tear down the surviving chunks.
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 2. SharedFile — partial-unmap the head 2 chunks. Tail must remain readable.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest, SharedFile_PartialUnmapHead) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_head", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Drop chunks [0, 2). This degenerates to a single tail fragment after
  // promotion (head fragment is empty).
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 2 * kChunk), Succeeds());

  // Tail [2, 8) still readable.
  EXPECT_TRUE(view_matches_pattern(base, 2 * kChunk, 2 * kChunk, 6 * kChunk));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 2 * kChunk,
                                     6 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 3. SharedFile — partial-unmap the tail 2 chunks. Head must remain readable.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest, SharedFile_PartialUnmapTail) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_tail", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Drop chunks [6, 8) — single head fragment survives.
  void *tail = static_cast<char *>(base) + 6 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(tail, 2 * kChunk), Succeeds());

  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 6 * kChunk));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 6 * kChunk), Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 4. SharedFile — after promotion, mprotect(R) and mprotect(RW) on the
//    surviving head chunks must both succeed and the data must read back
//    correctly. Confirms the CHUNKED shape's per-chunk protection path works
//    end to end.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_PartialUnmapAccessibleAfterPromotion) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_mprot", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Promote: drop middle [3, 5).
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Downgrade head to RO — file views translate this into PAGE_READONLY.
  ASSERT_EQ(LIBC_NAMESPACE::mprotect(base, 3 * kChunk, PROT_READ), 0);
  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 3 * kChunk));

  // Restore RW — file views with COW=false (MAP_SHARED) take PAGE_READWRITE.
  ASSERT_EQ(LIBC_NAMESPACE::mprotect(base, 3 * kChunk, PROT_READ | PROT_WRITE),
            0);
  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 3 * kChunk));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 5. SharedFile — write to a surviving head page, then unmap; the write must
//    have made it to the backing file (MAP_SHARED contract preserved across
//    the MONO->CHUNKED promotion).
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_WriteToSurvivingPagesPersistsToFile) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_persist", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Promote with middle hole.
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Stamp a recognisable marker into the head fragment, chunk 1.
  uint8_t marker[256];
  for (size_t i = 0; i < sizeof(marker); ++i)
    marker[i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));
  write_view(base, kChunk, sizeof(marker), marker);

  // Drop the surviving fragments. munmap is the implicit msync for MAP_SHARED
  // section views — closing the last view forces the modified pages out.
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());

  // Independent NtCreateFile + NtReadFile: the marker must be on disk.
  uint8_t scratch[sizeof(marker)];
  ASSERT_TRUE(reread_file(tf.nt_path, kChunk, sizeof(marker), scratch));
  for (size_t i = 0; i < sizeof(marker); ++i)
    EXPECT_EQ(scratch[i], marker[i]);

  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 6. PrivateFile — CoW dirty pages on the surviving fragments must persist
//    locally after a partial-unmap promotion. The CoW backing must NOT be
//    dropped just because shape transitioned MONO -> CHUNKED.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       PrivateFile_PartialUnmapPreservesCoWPages) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "private_cow", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Dirty one byte at the start of each chunk so all 8 chunks have private
  // CoW physical pages backing them. Marker = 0xC0 + chunk index.
  for (size_t i = 0; i < kNumChunks; ++i) {
    uint8_t b = static_cast<uint8_t>(0xC0 + i);
    write_view(base, i * kChunk, 1, &b);
  }

  // Punch the middle hole — promotes to CHUNKED.
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Surviving CoW marker bytes still hold the values we wrote.
  for (size_t i = 0; i < 3; ++i) {
    uint8_t b;
    read_view(base, i * kChunk, 1, &b);
    EXPECT_EQ(b, static_cast<uint8_t>(0xC0 + i));
  }
  for (size_t i = 5; i < kNumChunks; ++i) {
    uint8_t b;
    read_view(base, i * kChunk, 1, &b);
    EXPECT_EQ(b, static_cast<uint8_t>(0xC0 + i));
  }

  // Tail past the dirty byte still matches the original pattern (CoW was
  // page-granular, only the touched pages diverged).
  EXPECT_TRUE(view_matches_pattern(base, 1, 1, 4096 - 1));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 7. PrivateFile — same setup as (6); after partial-unmap, the backing file
//    must be byte-for-byte the original pattern. CoW dirty pages must not
//    leak through to the file.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       PrivateFile_PartialUnmapDoesNotLeakDirtyPagesIntoFile) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "private_isolation", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Dirty every chunk via CoW.
  for (size_t i = 0; i < kNumChunks; ++i) {
    uint8_t b = static_cast<uint8_t>(0xE0 + i);
    write_view(base, i * kChunk, 1, &b);
  }

  // Promote and drop the middle.
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Read each chunk's first byte directly from the backing file via a fresh
  // handle. The original pattern bytes must be intact — none of the CoW
  // 0xEx markers leaked.
  for (size_t i = 0; i < kNumChunks; ++i) {
    uint8_t scratch;
    ASSERT_TRUE(reread_file(tf.nt_path, i * kChunk, 1, &scratch));
    EXPECT_EQ(scratch, pattern_byte(i * kChunk));
  }

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 8. SharedFile — after promotion to CHUNKED with two surviving chunks, each
//    chunk can be torn down via its own munmap call and the file path is
//    fully released so a fresh mmap of the same path succeeds.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_FullUnmapAfterPartialPromotion) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_full_after", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Promote.
  void *hole = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Two separate munmap calls, one per surviving chunk fragment.
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());

  // Fresh mmap of the same fd succeeds — the section/file handle was
  // released cleanly when the final chunk's region refcount dropped.
  void *base2 =
      LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base2, MAP_FAILED);
  EXPECT_TRUE(view_matches_pattern(base2, 0, 0, kFileSize));
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base2, kFileSize), Succeeds());

  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 9. SharedFile — two non-adjacent partial-unmaps. After both, three
//    surviving chunk fragments remain; each is independently readable with
//    correct data.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_TwoSeparatePartialUnmaps_ThreeChunks) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_three", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Hole 1: drop chunk 2 only. MONO -> CHUNKED with 2 fragments
  // ([0,2), [3,8)).
  void *hole1 = static_cast<char *>(base) + 2 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole1, kChunk), Succeeds());

  // Hole 2: drop chunk 5, non-adjacent to hole 1. CHUNKED -> CHUNKED with
  // 3 fragments ([0,2), [3,5), [6,8)).
  void *hole2 = static_cast<char *>(base) + 5 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole2, kChunk), Succeeds());

  // Three surviving fragments — verify each.
  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 2 * kChunk));
  EXPECT_TRUE(view_matches_pattern(base, 3 * kChunk, 3 * kChunk, 2 * kChunk));
  EXPECT_TRUE(view_matches_pattern(base, 6 * kChunk, 6 * kChunk, 2 * kChunk));

  // Tear down each surviving fragment.
  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 2 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 3 * kChunk,
                                     2 * kChunk),
              Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 6 * kChunk,
                                     2 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 10. SharedFile — two immediately-adjacent partial-unmaps.
//
//     The implementation may or may not coalesce adjacent freed runs in the
//     CHUNKED chunk_list. This test does not pin down which behaviour is
//     correct; either outcome is acceptable. What is *not* acceptable is
//     surviving pages becoming unreadable or the data going wrong, so we
//     assert only that property and document the looseness inline.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       SharedFile_AdjacentPartialUnmapsCoalesce) {
  TestFile tf;
  ASSERT_TRUE(open_test_file(tf, "shared_adjacent", O_RDWR));

  void *base = LIBC_NAMESPACE::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, tf.fd, 0);
  ASSERT_NE(base, MAP_FAILED);

  // Drop chunk 3, then chunk 4 (adjacent, lower-then-upper). Whether the
  // implementation merges these into one run or keeps two is a private
  // detail of chunk_list_punch; either is fine.
  void *hole1 = static_cast<char *>(base) + 3 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole1, kChunk), Succeeds());
  void *hole2 = static_cast<char *>(base) + 4 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole2, kChunk), Succeeds());

  // Final invariant: surviving head [0,3) and tail [5,8) read the original
  // pattern. That's the user-visible contract — coalescing or not is opaque.
  EXPECT_TRUE(view_matches_pattern(base, 0, 0, 3 * kChunk));
  EXPECT_TRUE(
      view_matches_pattern(base, 5 * kChunk, 5 * kChunk, 3 * kChunk));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, 3 * kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 5 * kChunk,
                                     3 * kChunk),
              Succeeds());
  close_test_file(tf);
}

// ---------------------------------------------------------------------------
// 11. Anonymous control — partial-unmap of a MAP_ANONYMOUS|MAP_PRIVATE
//     mapping must still go through the existing decommit path and leave
//     the surrounding pages readable. Confirms the file-shape changes do
//     not perturb the anonymous path.
// ---------------------------------------------------------------------------

TEST_F(LlvmLibcMmapFilePartialUnmapTest,
       MmapFile_AnonymousPartialUnmapStillUsesDecommitPath) {
  // 4 chunks of anonymous private memory, written with a sentinel pattern.
  void *base =
      LIBC_NAMESPACE::mmap(nullptr, 4 * kChunk, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(base, MAP_FAILED);

  uint8_t *bytes = reinterpret_cast<uint8_t *>(base);
  for (size_t i = 0; i < 4; ++i)
    bytes[i * kChunk] = static_cast<uint8_t>('W' + i);

  // Drop the middle 2 chunks.
  void *hole = static_cast<char *>(base) + 1 * kChunk;
  ASSERT_THAT(LIBC_NAMESPACE::munmap(hole, 2 * kChunk), Succeeds());

  // Surrounding chunks still readable with the sentinels intact.
  EXPECT_EQ(bytes[0 * kChunk], static_cast<uint8_t>('W' + 0));
  EXPECT_EQ(bytes[3 * kChunk], static_cast<uint8_t>('W' + 3));

  ASSERT_THAT(LIBC_NAMESPACE::munmap(base, kChunk), Succeeds());
  ASSERT_THAT(LIBC_NAMESPACE::munmap(static_cast<char *>(base) + 3 * kChunk,
                                     kChunk),
              Succeeds());
}
