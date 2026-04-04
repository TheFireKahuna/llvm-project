//===-- r_debug rendezvous engine ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Maintains the exported `_r_debug` / `_r_debug_pe` rendezvous structures so
// cross-process debuggers (lldb-server, gdb-stub) can enumerate loaded
// modules without a Windows debug port.  All writes go through the
// two-phase state machine documented in r_debug.h.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/dlfcn/r_debug.h"

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// Exported rendezvous structures
//
// `_r_debug` is the glibc-compatible entry point; `_r_debug_pe` is our
// PE-aware extension.  Both live in the default data segment — writable
// throughout the process lifetime so the two-phase mutation protocol can
// flip `r_state` around every list edit.
//
// Initial r_brk / r_brk_addr values are populated at runtime in
// `rdebug_startup_init` because the address of `_r_debug_state` is
// subject to ASLR relocation; a static initializer relying on the
// function address would force an import-relocation-style startup write
// anyway.  Doing it ourselves keeps the data-section initializer purely
// constant and cleanly documents "no PE until bootstrap".
//===----------------------------------------------------------------------===//

extern "C" {

struct r_debug _r_debug = {
    /*r_version=*/1,
    /*r_map=*/nullptr,
    /*r_brk=*/0,
    /*r_state=*/RT_CONSISTENT,
    /*r_ldbase=*/0,
};

struct r_debug_pe _r_debug_pe = {
    /*magic=*/R_DEBUG_PE_MAGIC,
    /*version=*/1,
    /*pe_map=*/nullptr,
    /*libc_base=*/0,
    /*r_debug_addr=*/0,
    /*r_brk_addr=*/0,
    /*ntdll_base=*/0,
    /*exe_base=*/0,
    /*peb_addr=*/0,
};

// The breakpoint site.  Debuggers plant a software breakpoint here and
// re-walk `_r_debug.r_map` whenever it fires with `r_state == RT_CONSISTENT`.
//
// The function MUST NOT be inlined or folded with another identical empty
// function.  `[[gnu::used]]` keeps the symbol, the volatile asm defeats
// identical-COMDAT-folding at the linker, and the .def-driven export keeps
// the name resolvable by cross-process debuggers.
[[gnu::noinline, gnu::used]]
void _r_debug_state(void) {
  __asm__ volatile("" ::: "memory");
}

// glibc-compatibility alias.  Alias (not duplicate) so breakpoints on
// either name share an address — the debugger that traps on one sees the
// other fire too.
[[gnu::alias("_r_debug_state")]]
void _dl_debug_state(void);

} // extern "C"

namespace {

//===----------------------------------------------------------------------===//
// Pool
//
// Fixed-capacity array of same-sized nodes carved from one page_reserve.
// Each node embeds its `l_name` and `pdb_path` buffers inline so node
// lifetime owns string lifetime with no side allocations — a debugger
// reads the pointer and dereferences into the same node's memory.
//
// Intrusive freelist via `free.next`; occupancy tracked implicitly (a
// node on the freelist is free, everything else is live).
//
// Concurrency: mutations come from the DLL notification callback
// (serialized by the loader lock) and from initial seeding during
// Tier A bootstrap (single-threaded — no other thread exists yet);
// fork_reinit runs single-threaded in the child.  No internal pool
// lock is needed.
//===----------------------------------------------------------------------===//

constexpr int RDEBUG_MAX_NODES = 1024;
constexpr int RDEBUG_MAX_PATH = 260;
constexpr int RDEBUG_NAME_BUF_SIZE = RDEBUG_MAX_PATH * 3 + 1; // UTF-8 of full DLL path
constexpr int RDEBUG_PDB_BUF_SIZE = RDEBUG_MAX_PATH + 1;      // ASCII of PDB filename

struct RDebugNode {
  union {
    link_map_pe in_use;
    struct {
      RDebugNode *next_free;
    } free;
  };
  char name_buf[RDEBUG_NAME_BUF_SIZE];
  char pdb_buf[RDEBUG_PDB_BUF_SIZE];
};

constexpr size_t RDEBUG_NODE_SIZE = sizeof(RDebugNode);
static_assert(RDEBUG_NODE_SIZE >= sizeof(link_map_pe),
              "node must hold a link_map_pe");

struct RDebugPool {
  RDebugNode *base = nullptr;       // page_reserve base (null before first use)
  int reserved = 0;                 // total slots reserved (RDEBUG_MAX_NODES)
  int high_water = 0;               // slots touched (committed span in units of nodes)
  RDebugNode *free_head = nullptr;  // intrusive freelist of released nodes

  // Lazy one-shot reservation.  Returns false if the VA reservation
  // failed (address-space exhaustion, page-file quota).
  bool ensure_reserved() {
    if (base)
      return true;
    size_t bytes = RDEBUG_NODE_SIZE * RDEBUG_MAX_NODES;
    base = static_cast<RDebugNode *>(internal::page_reserve(bytes));
    if (!base)
      return false;
    reserved = RDEBUG_MAX_NODES;
    high_water = 0;
    free_head = nullptr;
    return true;
  }

  // Commit one more node's page range if the next slot lands past the
  // currently committed span.  Pages are 4 KiB so one commit covers
  // multiple nodes — amortised cost per allocation is sub-syscall.
  bool ensure_committed(int slot_idx) {
    if (slot_idx < high_water)
      return true;
    size_t old_bytes = static_cast<size_t>(high_water) * RDEBUG_NODE_SIZE;
    size_t new_bytes = (static_cast<size_t>(slot_idx) + 1) * RDEBUG_NODE_SIZE;
    // Round up to page boundary.
    constexpr size_t PAGE = 4096;
    new_bytes = (new_bytes + PAGE - 1) & ~(PAGE - 1);
    old_bytes = (old_bytes + PAGE - 1) & ~(PAGE - 1);
    if (new_bytes > old_bytes) {
      if (!internal::page_commit(reinterpret_cast<char *>(base) + old_bytes,
                                 new_bytes - old_bytes))
        return false;
    }
    high_water = slot_idx + 1;
    return true;
  }

  RDebugNode *alloc() {
    if (!ensure_reserved())
      return nullptr;
    if (free_head) {
      RDebugNode *n = free_head;
      free_head = n->free.next_free;
      // Caller will initialize in_use members.
      return n;
    }
    if (high_water >= reserved)
      return nullptr;
    int idx = high_water;
    if (!ensure_committed(idx))
      return nullptr;
    return &base[idx];
  }

  void release(RDebugNode *n) {
    n->free.next_free = free_head;
    free_head = n;
  }

  // Release every live node and reset the freelist.  High-water stays;
  // demand-committed pages remain committed for the lifetime of the
  // process (rebuild-from-PEB in fork is the only caller and reuses them).
  void reset_live() {
    free_head = nullptr;
    high_water = 0; // subsequent allocs start from slot 0 again
    // The pages remain committed — future allocs skip recommit.
  }
};

RDebugPool g_pool;

//===----------------------------------------------------------------------===//
// PE header walk helpers
//===----------------------------------------------------------------------===//

// IMAGE_DIRECTORY_ENTRY_DEBUG is defined in the PE spec at index 6; no
// constant for it in nt_types.h (not used elsewhere).  Keep local.
constexpr int RDEBUG_IMAGE_DIR_DEBUG = 6;
constexpr uint32_t RDEBUG_IMAGE_DEBUG_TYPE_CODEVIEW = 2;

struct ImageDebugDirEntry {
  uint32_t Characteristics;
  uint32_t TimeDateStamp;
  uint16_t MajorVersion;
  uint16_t MinorVersion;
  uint32_t Type;
  uint32_t SizeOfData;
  uint32_t AddressOfRawData;   // RVA relative to DllBase
  uint32_t PointerToRawData;   // file offset (unused at runtime)
};
static_assert(sizeof(ImageDebugDirEntry) == 28, "IMAGE_DEBUG_DIRECTORY size");

// RSDS record — CODEVIEW PDB70.  `Signature` is the raw 16-byte GUID
// (little-endian as stored in the PE); we pass the bytes through
// unchanged — the debugger knows how to format them.
struct CvInfoPdb70 {
  uint32_t CvSignature; // 'RSDS' = 0x53445352
  uint8_t  Signature[16];
  uint32_t Age;
  char     PdbFileName[1]; // null-terminated, variable length
};
static_assert(offsetof(CvInfoPdb70, PdbFileName) == 24,
              "CV_INFO_PDB70 layout");

constexpr uint32_t RSDS_SIGNATURE = 0x53445352u;

struct PeMetadata {
  uint32_t size_of_image = 0;
  uint32_t checksum = 0;
  uint16_t machine = 0;
  uint16_t dll_characteristics = 0;
  uintptr_t entry_point = 0;
  uintptr_t pdata_va = 0;
  uint32_t  pdata_size = 0;
  uint32_t  pdb_age = 0;
  uint8_t   pdb_guid[16] = {};
  const char *pdb_name = nullptr; // points into image memory; caller copies
  bool has_codeview = false;
};

// Safe-ish PE header parse.  Any malformed field short-circuits;
// partially-populated metadata is the graceful-degradation case.
// All reads happen while the image is mapped (callback holds loader
// lock; fork child reads from CoW mapping).
void parse_pe_metadata(PVOID dll_base, PeMetadata &out) {
  if (!dll_base)
    return;
  auto *nt_hdr = static_cast<IMAGE_NT_HEADERS64 *>(::RtlImageNtHeader(dll_base));
  if (!nt_hdr || nt_hdr->Signature != IMAGE_NT_SIGNATURE)
    return;

  const IMAGE_FILE_HEADER &fh = nt_hdr->FileHeader;
  const IMAGE_OPTIONAL_HEADER64 &oh = nt_hdr->OptionalHeader;
  auto base_addr = reinterpret_cast<uintptr_t>(dll_base);

  out.size_of_image = oh.SizeOfImage;
  out.checksum = oh.CheckSum;
  out.machine = fh.Machine;
  out.dll_characteristics = oh.DllCharacteristics;
  out.entry_point = oh.AddressOfEntryPoint
                        ? base_addr + oh.AddressOfEntryPoint
                        : 0;

  const IMAGE_DATA_DIRECTORY &pdata_dd =
      oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (pdata_dd.VirtualAddress && pdata_dd.Size) {
    out.pdata_va = base_addr + pdata_dd.VirtualAddress;
    out.pdata_size = pdata_dd.Size;
  }

  // Debug directory walk — find CODEVIEW (type 2) entry, pull RSDS.
  if (oh.NumberOfRvaAndSizes <= RDEBUG_IMAGE_DIR_DEBUG)
    return;
  const IMAGE_DATA_DIRECTORY &dbg_dd =
      oh.DataDirectory[RDEBUG_IMAGE_DIR_DEBUG];
  if (!dbg_dd.VirtualAddress || dbg_dd.Size < sizeof(ImageDebugDirEntry))
    return;

  auto *entries = reinterpret_cast<ImageDebugDirEntry *>(
      base_addr + dbg_dd.VirtualAddress);
  uint32_t count = dbg_dd.Size / sizeof(ImageDebugDirEntry);
  for (uint32_t i = 0; i < count; ++i) {
    const ImageDebugDirEntry &e = entries[i];
    if (e.Type != RDEBUG_IMAGE_DEBUG_TYPE_CODEVIEW)
      continue;
    if (!e.AddressOfRawData || e.SizeOfData < sizeof(CvInfoPdb70))
      continue;
    auto *cv = reinterpret_cast<CvInfoPdb70 *>(base_addr + e.AddressOfRawData);
    if (cv->CvSignature != RSDS_SIGNATURE)
      continue;
    for (int b = 0; b < 16; ++b)
      out.pdb_guid[b] = cv->Signature[b];
    out.pdb_age = cv->Age;
    out.pdb_name = cv->PdbFileName;
    out.has_codeview = true;
    break;
  }
}

// Locate the LDR entry for a DllBase.  Loader lock assumed held.
LDR_DATA_TABLE_ENTRY *find_ldr_entry(PVOID dll_base) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr)
    return nullptr;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    // InLoadOrderLinks at offset 0.
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == dll_base)
      return entry;
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// UTF-16 → UTF-8 copy for l_name (debugger reads C strings)
//
// Uses the existing `wide_to_utf8_n` helper from nt_string.h — same
// path the rest of the libc uses for PEB path conversions.  The source
// comes in as an `nt_wstring_view` so NUL-less UNICODE_STRINGs work
// without a separate length parameter.
//===----------------------------------------------------------------------===//

void copy_nt_wstring_view_to_utf8(windows::nt_wstring_view src,
                                  char *dst, size_t dst_size) {
  if (!dst_size)
    return;
  int n = windows::wide_to_utf8_n(src.data(), src.size(), dst, dst_size - 1);
  dst[n > 0 ? n : 0] = '\0';
}

//===----------------------------------------------------------------------===//
// ntdll handle resolution — matches the pattern used in nt_capabilities.cpp.
// Avoids manual name matching against LDR entries; the kernel's own loader
// bookkeeping gives us the base.
//===----------------------------------------------------------------------===//

PVOID resolve_ntdll_base() {
  windows::nt_wstring_view ntdll_name(u"ntdll.dll");
  PVOID h = nullptr;
  NTSTATUS st = ::LdrGetDllHandleByName(ntdll_name.unicode_string(), nullptr, &h);
  return NT_SUCCESS(st) ? h : nullptr;
}

//===----------------------------------------------------------------------===//
// Node population
//
// Snapshots every PE / LDR field into the node up-front so that later
// debugger reads never need to re-dereference the LDR entry (which may
// have moved between builds or been partially invalidated at unload).
//===----------------------------------------------------------------------===//

void populate_node(RDebugNode *n, PVOID dll_base, LDR_DATA_TABLE_ENTRY *ldr) {
  link_map_pe &pe = n->in_use;
  auto base_addr = reinterpret_cast<uintptr_t>(dll_base);

  // Common POSIX prefix.
  pe.base.l_addr = base_addr;
  pe.base.l_name = n->name_buf;
  pe.base.l_ld = nullptr;
  pe.base.l_next = nullptr;
  pe.base.l_prev = nullptr;

  // UTF-8 path from LDR FullDllName (or empty string if LDR is gone).
  if (ldr && ldr->FullDllName.Buffer && ldr->FullDllName.Length) {
    copy_nt_wstring_view_to_utf8(windows::nt_wstring_view(ldr->FullDllName),
                                 n->name_buf, RDEBUG_NAME_BUF_SIZE);
  } else {
    n->name_buf[0] = '\0';
  }

  // PE extension header.
  pe.pe_magic = R_DEBUG_PE_MAGIC;
  pe.reserved0 = 0;
  pe.pdb_path = nullptr;
  pe.pe_next = nullptr;
  pe.pe_prev = nullptr;
  pe.ldr_entry = reinterpret_cast<uintptr_t>(ldr);
  pe.load_flags = ldr ? ldr->Flags : 0;
  pe.tls_index = ldr ? static_cast<uint32_t>(ldr->TlsIndex) : 0xFFFFFFFFu;

  // PE header walk.
  PeMetadata md;
  parse_pe_metadata(dll_base, md);
  pe.size_of_image = md.size_of_image
                         ? md.size_of_image
                         : (ldr ? ldr->SizeOfImage : 0);
  pe.timestamp = ldr ? ldr->TimeDateStamp : 0;
  pe.checksum = md.checksum;
  pe.machine = md.machine;
  pe.dll_characteristics = md.dll_characteristics;
  pe.entry_point = md.entry_point;
  pe.pdata_va = md.pdata_va;
  pe.pdata_size = md.pdata_size;
  pe.pdb_age = md.pdb_age;
  for (int b = 0; b < 16; ++b)
    pe.pdb_guid[b] = md.pdb_guid[b];

  // PDB filename from CODEVIEW RSDS record — ASCII path (as stored in PE).
  if (md.has_codeview && md.pdb_name) {
    int i = 0;
    for (; i < RDEBUG_PDB_BUF_SIZE - 1 && md.pdb_name[i]; ++i)
      n->pdb_buf[i] = md.pdb_name[i];
    n->pdb_buf[i] = '\0';
    if (i > 0)
      pe.pdb_path = n->pdb_buf;
  } else {
    n->pdb_buf[0] = '\0';
  }
}

//===----------------------------------------------------------------------===//
// List operations
//
// Maintains two tails so appends are O(1).  Both lists are kept in the
// same order — every node is on both lists simultaneously.
//===----------------------------------------------------------------------===//

link_map *g_posix_tail = nullptr;
link_map_pe *g_pe_tail = nullptr;

void link_append(link_map_pe *pe) {
  // Append to POSIX list.
  pe->base.l_prev = g_posix_tail;
  pe->base.l_next = nullptr;
  if (g_posix_tail)
    g_posix_tail->l_next = &pe->base;
  else
    _r_debug.r_map = &pe->base;
  g_posix_tail = &pe->base;

  // Append to PE list.
  pe->pe_prev = g_pe_tail;
  pe->pe_next = nullptr;
  if (g_pe_tail)
    g_pe_tail->pe_next = pe;
  else
    _r_debug_pe.pe_map = pe;
  g_pe_tail = pe;
}

void link_unlink(link_map_pe *pe) {
  // Unlink from POSIX list.
  if (pe->base.l_prev)
    pe->base.l_prev->l_next = pe->base.l_next;
  else
    _r_debug.r_map = pe->base.l_next;
  if (pe->base.l_next)
    pe->base.l_next->l_prev = pe->base.l_prev;
  else
    g_posix_tail = pe->base.l_prev;

  // Unlink from PE list.
  if (pe->pe_prev)
    pe->pe_prev->pe_next = pe->pe_next;
  else
    _r_debug_pe.pe_map = pe->pe_next;
  if (pe->pe_next)
    pe->pe_next->pe_prev = pe->pe_prev;
  else
    g_pe_tail = pe->pe_prev;
}

link_map_pe *find_node_by_base(PVOID dll_base) {
  auto base_addr = reinterpret_cast<uintptr_t>(dll_base);
  for (link_map_pe *n = _r_debug_pe.pe_map; n; n = n->pe_next) {
    if (n->base.l_addr == base_addr)
      return n;
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Two-phase state-machine transitions
//
// The debugger only re-reads `r_map` after a transition to RT_CONSISTENT.
// The intermediate RT_ADD / RT_DELETE value tells it "an edit is in
// progress — don't walk the list yet".  Both `_r_debug_state()` calls
// fire regardless of debugger attachment; the cost is two empty
// function calls per DLL load/unload.
//===----------------------------------------------------------------------===//

void transition_begin(int state) {
  _r_debug.r_state = state;
  _r_debug_state();
}

void transition_end() {
  _r_debug.r_state = RT_CONSISTENT;
  _r_debug_state();
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

namespace internal {

void rdebug_on_dll_load(void *dll_base) {
  if (!dll_base)
    return;

  LDR_DATA_TABLE_ENTRY *ldr = find_ldr_entry(dll_base);

  // Skip if already present (e.g., redundant notification, which can
  // happen when other tools also hook the loader).
  if (find_node_by_base(dll_base))
    return;

  RDebugNode *node = g_pool.alloc();
  if (!node)
    return; // Pool full — debugger will simply miss this module.

  populate_node(node, dll_base, ldr);

  transition_begin(RT_ADD);
  link_append(&node->in_use);
  transition_end();
}

void rdebug_on_dll_unload(void *dll_base) {
  if (!dll_base)
    return;

  link_map_pe *node = find_node_by_base(dll_base);
  if (!node)
    return;

  transition_begin(RT_DELETE);
  link_unlink(node);
  transition_end();

  g_pool.release(reinterpret_cast<RDebugNode *>(node));
}

void rdebug_startup_init(void) {
  // Idempotent guard — only the first call populates the list.
  // Subsequent calls (e.g., a stray second init from a host program)
  // no-op cleanly.
  if (_r_debug.r_brk != 0)
    return;

  // Publish the breakpoint function address and cross-module pointers.
  _r_debug.r_brk = reinterpret_cast<uintptr_t>(&_r_debug_state);

  _r_debug_pe.r_debug_addr = reinterpret_cast<uintptr_t>(&_r_debug);
  _r_debug_pe.r_brk_addr = reinterpret_cast<uintptr_t>(&_r_debug_state);
  _r_debug_pe.peb_addr = reinterpret_cast<uintptr_t>(NtCurrentPeb());
  _r_debug_pe.exe_base =
      reinterpret_cast<uintptr_t>(NtCurrentPeb()->ImageBaseAddress);
  // c.dll base: address of this TU's code maps back via RtlPcToFileHeader.
  {
    PVOID base = nullptr;
    ::RtlPcToFileHeader(reinterpret_cast<PVOID>(&_r_debug_state), &base);
    _r_debug_pe.libc_base = reinterpret_cast<uintptr_t>(base);
  }

  // Cache ntdll base via the loader's name lookup — avoids a manual
  // UNICODE_STRING compare and shares the same resolution path used by
  // nt_capabilities.cpp.
  _r_debug_pe.ntdll_base = reinterpret_cast<uintptr_t>(resolve_ntdll_base());

  // Seed the list from PEB->Ldr.  Tier A is single-threaded, so no
  // concurrent LOAD can race the walk.  Bracket the bulk insert with
  // a single RT_ADD / RT_CONSISTENT pair — a debugger attaching post-
  // init walks the finished list anyway.
  transition_begin(RT_ADD);

  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (ldr) {
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
      auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
      if (!entry->DllBase)
        continue;

      RDebugNode *node = g_pool.alloc();
      if (!node)
        break; // Pool full — stop seeding; callback will track later loads.
      populate_node(node, entry->DllBase, entry);
      link_append(&node->in_use);
    }
  }

  transition_end();
}

void rdebug_fini(void) {
  // Drain to an empty consistent state.  A debugger that attaches in
  // the narrow window between fini and c.dll unmap sees "no modules"
  // rather than dangling pointers into just-freed pool pages.
  transition_begin(RT_DELETE);
  _r_debug.r_map = nullptr;
  _r_debug_pe.pe_map = nullptr;
  g_posix_tail = nullptr;
  g_pe_tail = nullptr;
  g_pool.reset_live();
  transition_end();

  _r_debug.r_brk = 0;
}

void rdebug_fork_reinit(void) {
  // The CoW'd pool and PEB are both valid in the child, but an
  // inconsistent parent-side state (e.g., parent fork()ed mid-callback
  // on another thread) could leave the list half-mutated.  Rebuild
  // from scratch — cheap, and unambiguous.
  _r_debug.r_map = nullptr;
  _r_debug_pe.pe_map = nullptr;
  g_posix_tail = nullptr;
  g_pe_tail = nullptr;
  g_pool.reset_live();

  // Refresh runtime-stamped PEB / exe / libc pointers (base addresses
  // are inherited, but being explicit avoids trusting fork CoW on
  // the rendezvous struct itself).
  _r_debug_pe.peb_addr = reinterpret_cast<uintptr_t>(NtCurrentPeb());
  _r_debug_pe.exe_base =
      reinterpret_cast<uintptr_t>(NtCurrentPeb()->ImageBaseAddress);

  transition_begin(RT_ADD);
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (ldr) {
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
      auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
      if (!entry->DllBase)
        continue;
      RDebugNode *node = g_pool.alloc();
      if (!node)
        break;
      populate_node(node, entry->DllBase, entry);
      link_append(&node->in_use);
    }
  }
  transition_end();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FORK_REINIT(rdebug,
                          ::LIBC_NAMESPACE::internal::kForkPrioRdebug,
                          &::LIBC_NAMESPACE::internal::rdebug_fork_reinit)
