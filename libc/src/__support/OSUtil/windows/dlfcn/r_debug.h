//===-- r_debug: GDB/LLDB solib rendezvous for NT-POSIX ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-process debugger module-enumeration protocol.
//
// ---- Discovery -----------------------------------------------------------
//   PE has no DT_DEBUG tag, so the debugger must find the rendezvous struct
//   by name. `_r_debug` and `_r_debug_pe` are exported from c.dll via
//   libc_entrypoints.def. The debugger reads the PE export table from a
//   remote memory read, resolves the symbol, and reads through.
//
// ---- POSIX-compatible view (glibc-identical) ----------------------------
//   `struct r_debug` + `struct link_map` are byte-for-byte the Linux layout
//   on x86-64. Any debugger that already speaks the ELF rendezvous (lldb's
//   DynamicLoaderPOSIXDYLD, gdb's solib-svr4) reads this view and sees a
//   complete list of loaded modules. `l_ld` is null on PE (no DT_DYNAMIC).
//
// ---- PE-aware view ------------------------------------------------------
//   `struct link_map_pe` starts with a full `struct link_map` at offset 0
//   (C common-initial-sequence), so a POSIX-only debugger can walk our
//   nodes through `r_map`. A PE-aware debugger that resolves `_r_debug_pe`
//   gets the extended record: PDB GUID/age/path (for symbol-server queries),
//   .pdata VA+size (for remote unwind without parsing PE headers),
//   TimeDateStamp + SizeOfImage + CheckSum (symbol-server fallback), Machine,
//   DllCharacteristics (CFG/HE-ASLR/DEP posture), AddressOfEntryPoint,
//   LDR_DATA_TABLE_ENTRY pointer, TLS index.
//
// ---- Protocol -----------------------------------------------------------
//   Every list mutation is bracketed by two calls to the breakpoint site:
//
//       r_state = RT_ADD / RT_DELETE ;  _r_debug_state() ;
//       <mutate list>                ;
//       r_state = RT_CONSISTENT      ;  _r_debug_state() ;
//
//   The debugger sets a breakpoint on `_r_debug_state`. On hit, it reads
//   `r_state`: a non-CONSISTENT value means "ignore, another event is
//   coming", CONSISTENT means "re-read r_map". This matches glibc ld.so.
//
// ---- Concurrency --------------------------------------------------------
//   All pool mutations happen from `rdebug_on_dll_load` / `_unload`, which
//   are called from the DLL notification callback — serialized by the
//   loader lock. Initial seeding runs during Tier A bootstrap, which is
//   single-threaded (no other thread can yet exist in the process). The
//   fork child is single-threaded during rdebug_fork_reinit, so no
//   synchronisation is needed there either. No internal pool lock is used.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_R_DEBUG_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_R_DEBUG_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

extern "C" {

// -----------------------------------------------------------------------
// POSIX-compatible rendezvous — identical layout to glibc's <link.h>.
// -----------------------------------------------------------------------

struct link_map {
  uintptr_t        l_addr;  // DllBase
  char            *l_name;  // UTF-8 full path, stable for node lifetime
  void            *l_ld;    // nullptr on PE (no DT_DYNAMIC)
  struct link_map *l_next;
  struct link_map *l_prev;
};

enum r_state_enum {
  RT_CONSISTENT = 0,
  RT_ADD        = 1,
  RT_DELETE     = 2,
};

struct r_debug {
  int              r_version; // 1
  struct link_map *r_map;     // head of doubly-linked list (null when empty)
  uintptr_t        r_brk;     // &_r_debug_state
  int              r_state;   // r_state_enum
  uintptr_t        r_ldbase;  // 0 on PE (no separate dynamic linker)
};

// The breakpoint site. Kept non-inlinable and unique so the debugger has
// a stable instruction address to trap. Debuggers also look up this
// symbol by name (`_r_debug_state` / `_dl_debug_state`) in addition to
// reading `_r_debug.r_brk`.
void _r_debug_state(void);

// glibc-compatibility alias: some older debugger stubs look up
// `_dl_debug_state` directly.
void _dl_debug_state(void);

// -----------------------------------------------------------------------
// PE-aware extension — parallel list carrying PE-specific module metadata.
// `link_map_pe.base` is at offset 0 (C common initial sequence) so POSIX
// debuggers that walk `_r_debug.r_map` see a valid `struct link_map`
// prefix. PE-aware debuggers resolve `_r_debug_pe` for the full record.
// -----------------------------------------------------------------------

#define R_DEBUG_PE_MAGIC 0x42444550u /* 'PEDB' little-endian */

struct link_map_pe {
  struct link_map base; // MUST be first — common initial sequence with link_map.

  uint32_t  pe_magic;            // R_DEBUG_PE_MAGIC — validation from remote reads.
  uint32_t  size_of_image;       // OptionalHeader.SizeOfImage
  uint32_t  timestamp;           // FileHeader.TimeDateStamp (symbol-server key)
  uint32_t  checksum;            // OptionalHeader.CheckSum
  uint16_t  machine;             // IMAGE_FILE_MACHINE_* (AMD64 / ARM64)
  uint16_t  dll_characteristics; // CFG / HE_ASLR / DEP / HIGH_ENTROPY_VA ...
  uint32_t  reserved0;           // pad to 8-byte boundary

  uintptr_t entry_point;         // Absolute VA of AddressOfEntryPoint (0 if none)
  uintptr_t pdata_va;            // Absolute VA of IMAGE_DIRECTORY_ENTRY_EXCEPTION
  uint32_t  pdata_size;          // Size in bytes
  uint32_t  pdb_age;             // From RSDS record (0 if no CODEVIEW entry)
  uint8_t   pdb_guid[16];        // Raw bytes — debugger formats for symbol server

  char     *pdb_path;            // UTF-8 PDB filename from RSDS (nullable)
  uintptr_t ldr_entry;           // PEB LDR_DATA_TABLE_ENTRY address (for Win-aware tools)
  uint32_t  tls_index;           // If the module has TLS: slot index; else UINT32_MAX
  uint32_t  load_flags;          // Snapshot of LDR entry Flags at load time

  struct link_map_pe *pe_next;   // Parallel linkage, same order as l_next.
  struct link_map_pe *pe_prev;
};

struct r_debug_pe {
  uint32_t  magic;                // R_DEBUG_PE_MAGIC
  uint32_t  version;              // 1
  struct link_map_pe *pe_map;     // Head of PE-extended list (null when empty).
  uintptr_t libc_base;            // DllBase of c.dll (this module).
  uintptr_t r_debug_addr;         // &_r_debug — self-pointer cross-check.
  uintptr_t r_brk_addr;           // &_r_debug_state — mirror of _r_debug.r_brk.
  uintptr_t ntdll_base;           // DllBase of ntdll.dll, cached at init.
  uintptr_t exe_base;             // PEB->ImageBaseAddress (main executable).
  uintptr_t peb_addr;             // NtCurrentPeb() — TEB-derived, stable.
};

} // extern "C"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// -----------------------------------------------------------------------
// Engine API — all functions assume single-writer context:
//   * rdebug_on_dll_load / _unload : loader lock held by the kernel
//     (LdrRegisterDllNotification callback invariant)
//   * rdebug_startup_init          : caller takes loader lock itself
//   * rdebug_fork_reinit           : child is single-threaded
//   * rdebug_fini                  : process shutdown, no concurrent callers
// -----------------------------------------------------------------------

// Seed the list from the current PEB loader state, then install the
// rendezvous struct's r_brk/r_map pointers. Idempotent — safe to call
// twice (second call is a no-op).
void rdebug_startup_init(void);

// Clear list, unmap pool. Called from veh_core_fini. After this the
// exported `_r_debug.r_map` is reset to null so any debugger attaching
// during teardown sees an empty, consistent list rather than dangling
// node pointers.
void rdebug_fini(void);

// Fork child re-initialization. The CoW'd pool + PEB give us the same
// DllBase values; we rebuild the list from the fresh PEB to drop any
// transient inconsistency from concurrent parent-side loads during fork.
void rdebug_fork_reinit(void);

// DLL load notification. `dll_base` is the DllBase from the notification
// data; we walk PEB->Ldr to retrieve full PE metadata.
void rdebug_on_dll_load(void *dll_base);

// DLL unload notification. Fires BEFORE the image is unmapped so the
// LDR entry and PE headers are still readable.
void rdebug_on_dll_unload(void *dll_base);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DLFCN_R_DEBUG_H
