//===-- Region snapshot for split-remap operations --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single-pass MBI walk with adjacent coalescing for the split-remap algorithm
// used by partial munmap, mremap shrink/grow/move, and mbind.
//
// Captures both protection and COW status per region in a unified walk:
//   - Consecutive MBI regions with identical (protect, is_cow) are merged
//   - Overflow returns -1 (caller must fail, not silently truncate)
//   - One NtQueryVirtualMemory walk instead of two
//
// COW handling — placeholder-preserving protocol:
//
//   Anonymous (pagefile-backed, PAGE_READWRITE):
//     No COW exists. Writes go to section pages, which survive unmap/remap.
//     Zero copies. has_cow() returns false.
//
//   File-backed MAP_PRIVATE (PAGE_WRITECOPY):
//     COW pages are PTE-level private copies lost on unmap. Instead of
//     remapping ALL pages from the section (which exposes stale file content
//     on dirty pages during the remap window), we:
//
//       1. WSEX scan to identify per-page dirty state (SharedOriginal=0)
//       2. Save only dirty page content + a bitmap to a temp buffer
//       3. After unmap+split, sub-split placeholders around dirty clusters
//       4. Remap clean ranges from section (WRITECOPY) — correct content
//       5. nt_pal::commit_replace dirty clusters → private READWRITE
//       6. memcpy saved content into private pages
//
//     Dirty pages transition directly from placeholder (NOACCESS, hardware-
//     enforced) to private committed with correct content — no wrong-content
//     window. 2 copies per dirty page instead of 3. Bitmap in the buffer
//     means no stack arrays and no hard cap on dirty cluster count.
//
// All internal VA uses the placeholder model.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REGION_SNAPSHOT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REGION_SNAPSHOT_H

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_region.h"
#include "src/__support/OSUtil/windows/memory/legacy/view_spec.h"
#include "src/__support/OSUtil/windows/alloc/section_region.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Region record and snapshot
//===----------------------------------------------------------------------===//

/// Unified region record: protection + COW status from a single MBI walk.
struct RegionRecord {
  uintptr_t offset; // From snapshot start.
  SIZE_T size;
  DWORD protect;    // Current protection (for replay after remap).
  bool is_cow;      // COW'd page — content lost on unmap.
};

/// Maximum records per snapshot. After coalescing, this bounds the number
/// of distinct (protect, is_cow) transitions. 128 entries = 3.5KB stack.
/// Overflow fails explicitly rather than silently truncating.
constexpr int MAX_REGION_RECORDS = 128;

/// Single-pass bulk MBI walk over [start, end) with adjacent coalescing.
/// Returns entry count on success, -1 on buffer overflow.
LIBC_INLINE int snapshot_regions(uintptr_t start, uintptr_t end,
                                 RegionRecord *out, int max_entries) {
  int count = 0;
  SIZE_T range_size = end - start;

  auto ws = byte_scratch(4096);
  if (!ws) return -1;
  nt_pal::RegionWalker walk(reinterpret_cast<void *>(start), range_size, ws.data(),
                    ws.size());
  while (walk.next()) {
    if (walk.entry->State != MEM_COMMIT)
      continue;

    DWORD prot = walk.entry->Protect;
    uintptr_t pos = reinterpret_cast<uintptr_t>(walk.chunk);
    SIZE_T region_size = walk.chunk_size;

    DWORD alloc_prot = walk.entry->AllocationProtect & 0xFF;
    DWORD cur_prot = prot & 0xFF;
    bool is_cow = (alloc_prot == PAGE_WRITECOPY ||
                   alloc_prot == PAGE_EXECUTE_WRITECOPY) &&
                  (cur_prot == PAGE_READWRITE ||
                   cur_prot == PAGE_EXECUTE_READWRITE);

    // Coalesce with previous if contiguous and identical attributes.
    if (count > 0) {
      RegionRecord &prev = out[count - 1];
      if (prev.protect == prot && prev.is_cow == is_cow &&
          prev.offset + prev.size == pos - start) {
        prev.size += region_size;
        continue;
      }
    }

    if (LIBC_UNLIKELY(count >= max_entries))
      return -1;

    out[count].offset = pos - start;
    out[count].size = region_size;
    out[count].protect = prot;
    out[count].is_cow = is_cow;
    ++count;
  }

  return count;
}

/// True if any record has COW'd pages.
LIBC_INLINE bool has_cow(const RegionRecord *records, int count) {
  for (int i = 0; i < count; ++i) {
    if (records[i].is_cow)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Protection replay
//===----------------------------------------------------------------------===//

/// Re-apply protection differences after remap. Skips records matching
/// base_prot (the common case — remap applies base_prot uniformly).
LIBC_INLINE void replay_protections(HANDLE process, uintptr_t base,
                                    DWORD base_prot,
                                    const RegionRecord *records, int count) {
  for (int i = 0; i < count; ++i) {
    if (records[i].protect == base_prot)
      continue;
    PVOID addr = reinterpret_cast<void *>(base + records[i].offset);
    SIZE_T size = records[i].size;
    ULONG old_prot;
    ::NtProtectVirtualMemory(process, &addr, &size, records[i].protect,
                             &old_prot);
  }
}

//===----------------------------------------------------------------------===//
// COW save buffer with embedded bitmap
//===----------------------------------------------------------------------===//
//
// Layout inside the SectionRegion temp buffer:
//
//   [CowSaveHeader]                          — 16 bytes
//   [bitmap: ceil(total_cow_pages/8) bytes]   — 1 bit per COW page
//   [padding to PAGE_SIZE boundary]
//   [dirty page 0 data]                       — 4KB
//   [dirty page 1 data]                       — 4KB
//   ...
//
// The bitmap is indexed by sequential page number across all is_cow records.
// A set bit means the page was dirty (COW'd) and its content is in the data
// section. Clusters are derived on the fly during remap by walking the bitmap.
//
// This design has no hard cap on dirty cluster count — the bitmap scales
// with the COW region size and lives in the allocated buffer, not the stack.

/// Header at offset 0 of the COW save buffer.
struct CowSaveHeader {
  SIZE_T total_cow_pages;  // Total pages across all is_cow records.
  SIZE_T dirty_page_count; // Number of set bits in the bitmap.
  SIZE_T data_offset;      // Byte offset from buffer start to page data.
  SIZE_T reserved;
};

/// Section-backed temp buffer for COW page content + bitmap.
using CowSaveBuffer = SectionRegion;

//===----------------------------------------------------------------------===//
// save_cow_pages — WSEX scan + bitmap + dirty page data in one pass
//===----------------------------------------------------------------------===//

/// Count total COW pages across is_cow records.
LIBC_INLINE SIZE_T count_cow_pages(const RegionRecord *records, int count) {
  const SIZE_T PAGE = get_page_size();
  SIZE_T total = 0;
  for (int i = 0; i < count; ++i) {
    if (records[i].is_cow)
      total += records[i].size / PAGE;
  }
  return total;
}

/// Scan dirty pages and save their content into a single buffer with bitmap.
///
/// Two detection strategies, chosen per-invocation:
///   1. Write-watch (MEM_WRITE_WATCH regions): `nt_pal::write_watch_get_reset`
///      returns the exact dirty page set in one syscall. Atomic query+reset
///      eliminates race windows. Only available on MEM_PRIVATE regions
///      allocated with MEM_WRITE_WATCH (MRI `MappedWriteWatch=1`).
///   2. WSEX scan (section views, legacy regions): batch
///      NtQueryVirtualMemory(MemoryWorkingSetExInformation) to identify
///      COW'd pages via SharedOriginal=0.
///
/// Returns a buffer containing the header, bitmap, and dirty page data.
/// On failure or if no dirty pages exist, returns {nullptr, nullptr}.
///
/// \p base is the fragment's virtual base address.
/// \p records / \p count are from snapshot_regions() for this fragment.
LIBC_INLINE CowSaveBuffer save_cow_pages(uintptr_t base,
                                          const RegionRecord *records,
                                          int count) {
  constexpr int BATCH = 256;
  const SIZE_T PAGE = get_page_size();
  CowSaveBuffer buf;

  SIZE_T total_cow_pages = count_cow_pages(records, count);
  if (total_cow_pages == 0)
    return buf;

  // Check if the first COW region supports write-watch (MEM_PRIVATE with
  // MEM_WRITE_WATCH). If so, use the faster nt_pal::write_watch_get_reset path.
  bool use_write_watch = false;
  for (int i = 0; i < count; ++i) {
    if (records[i].is_cow) {
      uintptr_t first_cow = base + records[i].offset;
      MEMORY_REGION_INFORMATION mri;
      if (nt_pal::query_region_mri(reinterpret_cast<void *>(first_cow), mri))
        use_write_watch = mri.MappedWriteWatch != 0;
      break;
    }
  }

  // First pass: build dirty page bitmap.
  SIZE_T bitmap_bytes = (total_cow_pages + 7) / 8;
  SIZE_T bitmap_alloc = round_up_to_align(
      sizeof(CowSaveHeader) + bitmap_bytes, PAGE);

  SectionRegion bitmap_region =
      SectionRegion::create_anon(static_cast<size_t>(bitmap_alloc));
  if (!bitmap_region)
    return buf;

  auto *hdr = static_cast<CowSaveHeader *>(bitmap_region.base());
  hdr->total_cow_pages = total_cow_pages;
  hdr->dirty_page_count = 0;
  hdr->data_offset = 0;
  hdr->reserved = 0;

  auto *bitmap = reinterpret_cast<unsigned char *>(hdr + 1);
  __builtin_memset(bitmap, 0, static_cast<size_t>(bitmap_bytes));

  SIZE_T page_idx = 0;
  SIZE_T dirty_count = 0;

  if (use_write_watch) {
    // Write-watch path: query+reset atomically, exact dirty set.
    // Allocate a temporary array for dirty page addresses.
    SIZE_T max_addrs = total_cow_pages;
    SIZE_T addrs_alloc = round_up_to_align(max_addrs * sizeof(void *), PAGE);
    SectionRegion addrs_region =
        SectionRegion::create_anon(static_cast<size_t>(addrs_alloc));
    if (!addrs_region) {
      bitmap_region.destroy();
      return buf;
    }
    auto **page_addrs = static_cast<void **>(addrs_region.base());

    for (int r = 0; r < count; ++r) {
      if (!records[r].is_cow)
        continue;

      uintptr_t region_start = base + records[r].offset;
      SIZE_T region_size = records[r].size;
      SIZE_T pages_in_record = region_size / PAGE;

      // Query dirty pages for this COW region.
      SIZE_T found = nt_pal::write_watch_get_reset(
          reinterpret_cast<void *>(region_start), region_size, page_addrs,
          static_cast<ULONG_PTR>(pages_in_record));

      // Build bitmap from the returned dirty addresses.
      for (SIZE_T i = 0; i < found; ++i) {
        uintptr_t dirty_addr = reinterpret_cast<uintptr_t>(page_addrs[i]);
        SIZE_T page_in_record = (dirty_addr - region_start) / PAGE;
        SIZE_T idx = page_idx + page_in_record;
        bitmap[idx / 8] |= static_cast<unsigned char>(1u << (idx % 8));
        dirty_count++;
      }

      page_idx += pages_in_record;
    }

    addrs_region.destroy();
  } else {
    // WSEX scan path: batch query working set extended information.
    MEMORY_WORKING_SET_EX_INFORMATION wsex_batch[BATCH];

    for (int r = 0; r < count; ++r) {
      if (!records[r].is_cow)
        continue;

      uintptr_t region_start = base + records[r].offset;
      uintptr_t region_end = region_start + records[r].size;
      char *cursor = reinterpret_cast<char *>(region_start);
      char *end = reinterpret_cast<char *>(region_end);

      while (cursor < end) {
        int n = 0;
        SIZE_T batch_page_idx = page_idx;
        for (; n < BATCH && cursor < end; n++, cursor += PAGE) {
          wsex_batch[n].VirtualAddress = cursor;
          wsex_batch[n].VirtualAttributes.Flags = 0;
        }

        NTSTATUS st = ::NtQueryVirtualMemory(
            NtCurrentProcess(), nullptr, MemoryWorkingSetExInformation,
            wsex_batch, static_cast<SIZE_T>(n) * sizeof(wsex_batch[0]),
            nullptr);

        if (NT_ERROR(st)) {
          // Conservative: mark all remaining pages in this record as dirty.
          for (int i = 0; i < n; i++) {
            SIZE_T idx = batch_page_idx + static_cast<SIZE_T>(i);
            bitmap[idx / 8] |= static_cast<unsigned char>(1u << (idx % 8));
            dirty_count++;
          }
          while (cursor < end) {
            bitmap[page_idx / 8] |=
                static_cast<unsigned char>(1u << (page_idx % 8));
            dirty_count++;
            page_idx++;
            cursor += PAGE;
          }
          continue;
        }

        for (int i = 0; i < n; i++) {
          SIZE_T idx = batch_page_idx + static_cast<SIZE_T>(i);
          ULONG_PTR flags = wsex_batch[i].VirtualAttributes.Flags;
          bool dirty;
          if (flags & 1) {
            // Valid page: SharedOriginal=0 means COW'd.
            dirty = ((flags >> 30) & 1) == 0;
          } else {
            // Invalid page: ModifiedList=1 AND SharedOriginal=0.
            dirty = ((flags >> 27) & 1) != 0 && ((flags >> 30) & 1) == 0;
          }
          if (dirty) {
            bitmap[idx / 8] |= static_cast<unsigned char>(1u << (idx % 8));
            dirty_count++;
          }
        }

        page_idx = batch_page_idx + static_cast<SIZE_T>(n);
      }
    }
  }

  hdr->dirty_page_count = dirty_count;

  if (dirty_count == 0) {
    bitmap_region.destroy();
    return buf;
  }

  // Allocate the final buffer: header + bitmap + page data.
  SIZE_T data_offset = bitmap_alloc; // Page-aligned start for page data.
  SIZE_T total_size = data_offset + dirty_count * PAGE;

  buf = SectionRegion::create_anon(static_cast<size_t>(total_size));
  if (!buf) {
    bitmap_region.destroy();
    return buf;
  }

  // Copy header + bitmap into final buffer.
  auto *final_hdr = static_cast<CowSaveHeader *>(buf.base());
  __builtin_memcpy(final_hdr, hdr,
                    static_cast<size_t>(sizeof(CowSaveHeader) + bitmap_bytes));
  final_hdr->data_offset = data_offset;

  // Done with the temp bitmap region.
  bitmap_region.destroy();

  // Second pass: copy dirty page data into the final buffer.
  char *dst = static_cast<char *>(buf.base()) + data_offset;
  auto *final_bitmap = reinterpret_cast<const unsigned char *>(final_hdr + 1);
  page_idx = 0;

  for (int r = 0; r < count; ++r) {
    if (!records[r].is_cow)
      continue;

    uintptr_t region_start = base + records[r].offset;
    SIZE_T pages_in_record = records[r].size / PAGE;

    for (SIZE_T p = 0; p < pages_in_record; ++p) {
      if (final_bitmap[page_idx / 8] & (1u << (page_idx % 8))) {
        const char *src = reinterpret_cast<const char *>(
            region_start + p * PAGE);
        __builtin_memcpy(dst, src, PAGE);
        dst += PAGE;
      }
      page_idx++;
    }
  }

  return buf;
}

/// Free a COW save buffer without restoring (error cleanup).
LIBC_INLINE void free_cow_save(CowSaveBuffer &buf) { buf.destroy(); }

/// Restore dirty page content from a save buffer into a remapped view at
/// target_base. Used when the remap is handled externally (e.g., mremap
/// move with slice-based remapping) and we just need to write saved content
/// on top. Writes trigger kernel COW on WRITECOPY pages. Only dirty pages
/// (bitmap bit set) are written — clean pages are left untouched.
LIBC_INLINE void restore_cow_pages(uintptr_t target_base,
                                    const RegionRecord *records, int count,
                                    CowSaveBuffer &buf) {
  if (!buf.base() || count == 0)
    return;

  const SIZE_T PAGE = get_page_size();
  auto *hdr = static_cast<const CowSaveHeader *>(buf.base());
  auto *bitmap = reinterpret_cast<const unsigned char *>(hdr + 1);
  const char *data =
      static_cast<const char *>(buf.base()) + hdr->data_offset;

  SIZE_T bmp_idx = 0;
  for (int r = 0; r < count; ++r) {
    if (!records[r].is_cow)
      continue;

    uintptr_t rec_start = target_base + records[r].offset;
    SIZE_T pages_in_rec = records[r].size / PAGE;

    for (SIZE_T p = 0; p < pages_in_rec; ++p) {
      if (bitmap[bmp_idx / 8] & (1u << (bmp_idx % 8))) {
        void *dst = reinterpret_cast<void *>(rec_start + p * PAGE);
        __builtin_memcpy(dst, data, PAGE);
        data += PAGE;
      }
      bmp_idx++;
    }
  }

  buf.destroy();
}

//===----------------------------------------------------------------------===//
// CowContext — RAII encapsulation of the COW save/restore lifecycle
//===----------------------------------------------------------------------===//
//
// Replaces the repeated has_cow → save_cow_pages → validate → restore pattern
// at four call sites (RemapGuard, RemapTransaction, move_section,
// grow_section_in_place).
//
// prepare() scans for dirty COW pages and saves their content. Returns false
// only if COW pages exist but the save buffer could not be allocated — callers
// must fail the operation (not silently lose data).
//
// restore() writes saved content into a (possibly relocated) view. Consumes
// the context — subsequent calls are no-ops. The destructor releases the
// buffer if restore() was never called (error cleanup path).

class CowContext {
  CowSaveBuffer buf_ = {};
  bool captured_ = false;

public:
  LIBC_INLINE CowContext() = default;
  LIBC_INLINE ~CowContext() { release(); }

  LIBC_INLINE CowContext(CowContext &&o) noexcept
      : buf_(cpp::move(o.buf_)), captured_(o.captured_) {
    o.captured_ = false;
  }
  LIBC_INLINE CowContext &operator=(CowContext &&o) noexcept {
    if (this != &o) {
      release();
      buf_ = cpp::move(o.buf_);
      captured_ = o.captured_;
      o.captured_ = false;
    }
    return *this;
  }
  CowContext(const CowContext &) = delete;
  CowContext &operator=(const CowContext &) = delete;

  /// Scan for COW pages and save dirty content. Returns false only if
  /// COW pages exist but the save buffer could not be allocated.
  [[nodiscard]] LIBC_INLINE bool prepare(uintptr_t base, const RegionRecord *records,
                                         int count) {
    if (!has_cow(records, count))
      return true;
    buf_ = save_cow_pages(base, records, count);
    if (!buf_ && count_cow_pages(records, count) > 0)
      return false;
    captured_ = static_cast<bool>(buf_);
    return true;
  }

  /// Whether dirty COW page content was captured.
  LIBC_INLINE explicit operator bool() const { return captured_; }

  /// Access the underlying buffer (for remap_with_cow_splits).
  LIBC_INLINE const CowSaveBuffer &buffer() const { return buf_; }

  /// Restore dirty pages into a (possibly relocated) view. Consumes the
  /// context — after this call, operator bool() returns false.
  LIBC_INLINE void restore(uintptr_t target_base, const RegionRecord *records,
                           int count) {
    if (captured_) {
      restore_cow_pages(target_base, records, count, buf_);
      captured_ = false;
    }
  }

  /// Release resources without restoring.
  LIBC_INLINE void release() {
    if (captured_) {
      free_cow_save(buf_);
      captured_ = false;
    }
  }
};

//===----------------------------------------------------------------------===//
// Placeholder-preserving remap with COW splits
//===----------------------------------------------------------------------===//

/// Remap a kept fragment using the placeholder-preserving COW protocol.
/// Clean ranges are remapped from the section; dirty clusters stay as
/// placeholders then become private committed with saved content.
///
/// The fragment's placeholder must already exist at
/// [frag_base, frag_base + frag_size). The bitmap in cow_buf identifies
/// dirty pages; clusters are derived on the fly — no hard cap.
///
/// \p records / \p rec_count are the snapshot for this fragment (for
/// replay_protections on section-remapped ranges).
///
/// Returns true on success. On failure, caller must handle cleanup of
/// whatever placeholders/views remain.
LIBC_INLINE bool remap_with_cow_splits(
    uintptr_t frag_base, SIZE_T frag_size, const ViewSpec &spec,
    const CowSaveBuffer &cow_buf, const RegionRecord *records, int rec_count) {

  const SIZE_T PAGE = get_page_size();
  auto *hdr = static_cast<const CowSaveHeader *>(cow_buf.base());
  auto *bitmap = reinterpret_cast<const unsigned char *>(hdr + 1);
  const char *page_data =
      static_cast<const char *>(cow_buf.base()) + hdr->data_offset;
  (void)hdr->total_cow_pages; // Available for diagnostics if needed.

  // Build a map from bitmap index to absolute address. The bitmap covers
  // all is_cow record pages sequentially, but those records may not be
  // contiguous within the fragment (non-COW records can interleave).
  // We walk the bitmap and records together to resolve addresses.
  //
  // Phase 1: Split the placeholder. Walk the bitmap to find dirty cluster
  // boundaries (transitions from clean→dirty and dirty→clean), and split
  // the placeholder at each boundary.

  // We need the absolute address of each COW page. Build this by walking
  // the records in the same order as the bitmap.

  // Helper lambda: iterate bitmap pages with their absolute addresses.
  // For each is_cow record, pages are sequential starting at
  // frag_base + record.offset.

  // Single left-to-right walk that splits AND remaps/commits at each
  // clean↔dirty transition. At each boundary:
  //   clean→dirty: split, then remap the clean range just completed
  //   dirty→clean: split, then commit+memcpy the dirty cluster just completed
  // Trailing range handled after the loop.
  SIZE_T bmp_idx = 0;

  // Track the current run.
  uintptr_t run_start = frag_base;
  bool run_is_dirty = false;
  bool run_started = false;
  const char *data_cursor = page_data;

  // Helper: remap a clean range from the section.
  auto remap_clean = [&](uintptr_t start, SIZE_T size) -> bool {
    ViewSpec range_spec = spec.at_offset(start - frag_base);
    NTSTATUS st = range_spec.map_into(reinterpret_cast<void *>(start), size);
    return NT_SUCCESS(st);
  };

  // Helper: commit a dirty cluster and copy saved content.
  auto commit_dirty = [&](uintptr_t start, SIZE_T size) -> bool {
    NTSTATUS st = nt_pal::commit_replace(
        reinterpret_cast<void *>(start), size, PAGE_READWRITE);
    if (NT_ERROR(st))
      return false;
    __builtin_memcpy(reinterpret_cast<void *>(start), data_cursor, size);
    data_cursor += size;
    return true;
  };

  // Helper: complete the current run — split at addr and remap/commit.
  auto complete_run = [&](uintptr_t addr) -> bool {
    if (!run_started)
      return true;
    SIZE_T run_size = addr - run_start;
    if (run_size == 0)
      return true;

    // Split at addr if it's not at the fragment end.
    if (addr < frag_base + frag_size) {
      if (!nt_pal::split_placeholder(reinterpret_cast<void *>(run_start), run_size))
        return false;
    }

    if (run_is_dirty)
      return commit_dirty(run_start, run_size);
    else
      return remap_clean(run_start, run_size);
  };

  // Walk all records (COW and non-COW) left to right.
  for (int r = 0; r < rec_count; ++r) {
    uintptr_t rec_start = frag_base + records[r].offset;

    if (!records[r].is_cow) {
      // Non-COW record: always clean. Check for transition from dirty.
      if (run_started && run_is_dirty) {
        if (!complete_run(rec_start))
          return false;
        run_start = rec_start;
        run_is_dirty = false;
      }
      if (!run_started) {
        run_start = rec_start;
        run_is_dirty = false;
        run_started = true;
      }
      // Extend the clean run through this record.
      continue;
    }

    // COW record: walk page by page using the bitmap.
    SIZE_T pages_in_rec = records[r].size / PAGE;
    for (SIZE_T p = 0; p < pages_in_rec; ++p) {
      bool dirty = (bitmap[bmp_idx / 8] & (1u << (bmp_idx % 8))) != 0;
      uintptr_t page_addr = rec_start + p * PAGE;

      if (!run_started) {
        run_start = page_addr;
        run_is_dirty = dirty;
        run_started = true;
      } else if (dirty != run_is_dirty) {
        // Transition — complete the previous run.
        if (!complete_run(page_addr))
          return false;
        run_start = page_addr;
        run_is_dirty = dirty;
      }
      // Otherwise: extend current run.
      bmp_idx++;
    }
  }

  // Complete the trailing run.
  if (!complete_run(frag_base + frag_size))
    return false;

  // Replay protections on section-remapped (clean) ranges. Private
  // committed dirty pages already have PAGE_READWRITE.
  replay_protections(NtCurrentProcess(), frag_base, spec.prot, records,
                     rec_count);

  return true;
}

/// Simple remap without COW — remaps the entire fragment from the section.
/// Used when has_cow() returns false (anonymous or no dirty pages).
LIBC_INLINE bool remap_fragment(uintptr_t frag_base, SIZE_T frag_size,
                                const ViewSpec &spec,
                                const RegionRecord *records, int rec_count) {
  NTSTATUS st = spec.map_into(reinterpret_cast<void *>(frag_base), frag_size);
  if (NT_ERROR(st))
    return false;

  replay_protections(NtCurrentProcess(), frag_base, spec.prot, records,
                     rec_count);
  return true;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REGION_SNAPSHOT_H
