# NT API declarations (ntdll.dll) - foundation layer
# Contains basic Windows types, NT structures, and NT API declarations.
# This is the lowest-level header, mirroring how ntdll.dll is the
# foundation that kernel32.dll builds upon.
add_header_library(
  ntdll
  HDRS
    ntdll.h
  DEPENDS
    libc.src.__support.macros.config
)

# Misuse-invariant COFF/PE section registry primitive.
# Foundation for link-time dispatch tables ($A/$M/$Z three-letter layout,
# macro-owned section names, runtime read-only verification).
add_object_library(
  section_registry
  SRCS
    section_registry.cpp
  HDRS
    section_registry.h
    libc_fork_registry.h
  DEPENDS
    .ntdll
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Single-flight CAS-gated lazy init helper for Tier B subsystems.
# Header-only; instantiated per subsystem via template parameter.
add_header_library(
  lazy_init
  HDRS
    lazy_init.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# `.libclzr` section registry — holds a reset-thunk pointer for
# every LazyInit<> that needs its gate cleared on exec. Walker called
# from exec_self_hollow() Phase 5 after per-subsystem fork_reinit hooks.
add_object_library(
  lazy_init_reset
  SRCS
    lazy_init_reset.cpp
  HDRS
    lazy_init_reset.h
  DEPENDS
    .section_registry
    .process_control_block
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
)

# `.libcfin` section registry — holds a teardown thunk for every
# subsystem that owns reclaimable state at FreeLibrary time. Walked once
# in reverse phase order from __libc_dll_fini(). Replaces the explicit
# reverse-of-init list in libc_subsystem_fini.cpp; subsystems register
# next to their fini definition via LIBC_REGISTER_FINI(phase, tag, &fn).
add_object_library(
  libc_fini_registry
  SRCS
    libc_fini_registry.cpp
  HDRS
    libc_fini_registry.h
  DEPENDS
    .section_registry
    .process_control_block
    .ntdll
    windows_ntdll
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
    libc.src.setjmp.setjmp
)

# Per-image libc teardown descriptor ("__libc_module_block"). A
# libc-linked DLL that wants its own .libcfin / .libclzr swept on
# FreeLibrary exports one of these, and veh_core's dll_notify_callback
# resolves it via LdrGetProcedureAddress on UNLOAD. Header-only; its
# sole cost is the resolver + the macro that emits the export.
add_header_library(
  module_block
  HDRS
    module_block.h
  DEPENDS
    .lazy_init_reset
    .libc_fini_registry
    .ntdll
    .section_registry
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Native APC helpers — thin wrappers around NtQueueApcThreadEx2.
add_header_library(
  apc
  HDRS
    apc.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
)

# Raw page allocation — depends only on ntdll, safe for freestanding use.
# Used by subsystems that cannot depend on mmap or malloc (signal pools,
# atexit, fd_table). Must stay header-only — many fundamental libraries
# (windows_util, process_control_block, brk_state) depend on it, so
# promoting it to an object library would create transitive-depend
# cycles through those bases.
add_header_library(
  page_alloc
  HDRS
    alloc/legacy/page_alloc.h
  DEPENDS
    .ntdll
    libc.src.__support.common
)

# ---- Layer 0 PAL (`nt_pal::`) -----------------------------------------------
# Sole NT primitive surface for every later layer of the Phase 1 memory
# architecture (NTPOSIX_MEMORY_ARCHITECTURE_DESIGN §6.0). One header per
# operation family; one object library that probes process cookie +
# SeLockMemoryPrivilege at libc init and re-probes after fork.

# `nt_pal/nt_pal_process_state.h` is a pure-leaf header — types only,
# included by `process_control_block.h` to embed the PAL state in the
# PCB. Keeping it as its own header_library prevents the PCB→nt_pal_state
# include cycle (`pal_state.h` reaches g_pcb through this struct's PCB
# slot, which means `pal_state.h` itself can't be on the PCB include
# path).
add_header_library(
  nt_pal_process_state
  HDRS
    nt_pal/nt_pal_process_state.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_state
  HDRS
    nt_pal/pal_state.h
  DEPENDS
    .nt_pal_process_state
    .process_control_block
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_placeholder
  HDRS
    nt_pal/placeholder.h
  DEPENDS
    .ntdll
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_section
  HDRS
    nt_pal/section.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_protect
  HDRS
    nt_pal/protect.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_write_watch
  HDRS
    nt_pal/write_watch.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_large_pages
  HDRS
    nt_pal/large_pages.h
  DEPENDS
    .ntdll
    .nt_pal_state
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_query
  HDRS
    nt_pal/query.h
  DEPENDS
    .ntdll
    .page_alloc
    .nt_pal_placeholder
    .nt_pal_write_watch
    libc.src.__support.common
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  nt_pal_lock
  HDRS
    nt_pal/lock.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Aggregate header-only umbrella so callers can depend on a single
# `.nt_pal` target instead of listing every sub-library. Each sub-header
# is already tracked by its own header_library above; the umbrella only
# pins one HDRS entry (`pal_state.h`) for CMake to anchor the INTERFACE
# library against — listing the entire HDRS set here, especially the
# meta-`nt_pal/nt_pal.h`, made `target_sources(... INTERFACE ...)`
# propagate relative paths into consuming targets in sibling directories
# and the CMake Generate step then failed to resolve the file.
add_header_library(
  nt_pal
  HDRS
    nt_pal/pal_state.h
  DEPENDS
    .nt_pal_process_state
    .nt_pal_state
    .nt_pal_placeholder
    .nt_pal_section
    .nt_pal_protect
    .nt_pal_write_watch
    .nt_pal_large_pages
    .nt_pal_query
    .nt_pal_lock
)

# Object library: probe state at libc init (.libcmem$P0) and re-probe
# at fork (LIBC_REGISTER_FORK_REINIT).
add_object_library(
  nt_pal_init
  SRCS
    nt_pal/pal_init.cpp
    nt_pal/pal_fork.cpp
  DEPENDS
    .memory_primitives_registry
    .nt_pal_large_pages
    .nt_pal_state
    .ntdll
    .process_control_block
    .section_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

# ---- Shared allocator sub-primitives ----------------------------------------
# Scaffolding layer factored out of SlabPool / IndexedPool / thread_scratch.
# No bodies migrated yet; each target is header-only and standalone. When
# bodies land, dependents of the legacy allocators will pick these up
# transitively — no CMake churn needed for callers.

# Guarded VA region: placeholder reserve + commit subset + leading/trailing
# guard pages. Replaces three independent implementations in slab_pool.h,
# indexed_pool.h, and thread_scratch.h.
add_header_library(
  alloc_guarded_region
  HDRS
    alloc/primitives/guarded_region.h
  DEPENDS
    .page_alloc
    libc.src.__support.common
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Fixed-size occupancy bitmap with tzcnt allocation and blsr iteration.
# AtomicBitmap<N> for lock-free callers, RelaxedBitmap<N> for lock-held.
add_header_library(
  alloc_occupancy_bitmap
  HDRS
    alloc/primitives/occupancy_bitmap.h
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# CSPRNG-seeded XOR canary derivation + freelist encoding. Unifies
# ProcessPrng seed + address-XOR canary pattern from SlabPool / thread_scratch.
add_header_library(
  alloc_canary_seed
  HDRS
    alloc/primitives/canary_seed.h
  DEPENDS
    .bcryptprimitives
    libc.src.__support.common
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Three-state (UNINIT/INITIALIZING/READY) one-shot init latch. Composes
# spin_wait for the hardware-adaptive (UMWAIT/MWAITX) wait_ready path.
add_header_library(
  alloc_init_latch
  HDRS
    alloc/primitives/init_latch.h
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.spin_wait
)

# Owning section + mapped view convenience wrapper for internal infrastructure.
add_header_library(
  section_region
  HDRS
    alloc/section_region.h
  DEPENDS
    .page_alloc
    .ntdll
    libc.src.__support.CPP.utility
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Direct TEB TLS slot access — single-instruction TLS reads/writes.
add_header_library(
  teb_tls
  HDRS
    tls/teb_tls.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.macros.config
)

# Centralised thread-exit TLS cleanup registry.
add_header_library(
  tls_cleanup
  HDRS
    tls/tls_cleanup.h
  DEPENDS
    .teb_tls
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Single extern-C entry point invoked by crt_tls.obj's `.CRT$XLC` callback
# on DLL_THREAD_DETACH / DLL_PROCESS_DETACH. Keeps the FaultGuard
# machinery and the namespaced internals it touches (g_pcb,
# __llvm_libc_setjmp, tls_cleanup_run_all) on the c.dll side so
# consumer EXE startup objects don't take cross-module references
# on libc internals.
add_object_library(
  tls_thread_detach
  SRCS
    tls/tls_thread_detach.cpp
  DEPENDS
    .process_control_block
    .tls_cleanup
    libc.src.__support.macros.config
    libc.src.setjmp.setjmp
)

# FLS-compatible shim backed by TEB TLS (migration only).
add_header_library(
  teb_fls
  HDRS
    tls/teb_fls.h
  DEPENDS
    .teb_tls
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# bcryptprimitives.dll declarations (ProcessPrng).
add_header_library(
  bcryptprimitives
  HDRS
    bcryptprimitives.h
  DEPENDS
    libc.hdr.types.size_t
)

# Slab pool — runtime-configurable slab allocator with per-slab freelists.
# Header-only; depends on page_alloc for placeholder primitives and
# bcryptprimitives for ProcessPrng.
add_header_library(
  slab_pool
  HDRS
    alloc/legacy/slab_pool.h
  DEPENDS
    .alloc_canary_seed
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .page_alloc
    .bcryptprimitives
    .teb_tls
    .tls_cleanup
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.spin_wait
)

# Per-thread demand-commit scratch allocator for path conversion buffers.
# Replaces stack-allocated WCHAR[MAX_NT_PATH_WCHARS] (64 KB) arrays that
# cause STATUS_STACK_OVERFLOW on worker threads with small initial commits.
# The .cpp carries the LIBC_REGISTER_FINI thunk that returns the scratch
# TLS slot to the PEB TlsBitmap on DLL_PROCESS_DETACH.
add_object_library(
  thread_scratch
  SRCS
    alloc/thread_scratch.cpp
  HDRS
    alloc/thread_scratch.h
  DEPENDS
    .alloc_canary_seed
    .alloc_guarded_region
    .crystalline_local_state
    .crystalline_registry
    .libc_fini_registry
    .mapping_table
    .page_alloc
    .page_size
    .bcryptprimitives
    .teb_tls
    .tls_cleanup
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.__support.threads.windows.spin_wait
)

# Global slab registry — two-level radix bitmap of active slab bases.
# Shared by all SlabPool instances for free-path identification.
add_object_library(
  slab_registry
  SRCS
    alloc/legacy/slab_registry.cpp
  DEPENDS
    .slab_pool
    libc.src.__support.macros.config
)

# Substrate arena registry — three-level radix bitmap keyed on addr >> 16.
# Answers "is this pointer inside any substrate arena?" in three atomic
# loads. Membership check must succeed *before* any release() dereferences
# a header derived from the caller's pointer, so this target has no
# substrate dep — the substrate depends on it.
add_header_library(
  alloc_substrate_registry
  HDRS
    alloc/legacy/substrate_registry.h
  DEPENDS
    .alloc_init_latch
    .page_alloc
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# VaSubstrate — unified placeholder-backed VA allocator shared by all
# internal allocators (SlabPool, IndexedPool, MappingTable radix pages,
# ThreadScratch). Each arena is one LIBC_INTERNAL sentinel in the mapping
# table; sub-slot churn is invisible to the table. Lock-free hot path;
# epoch-gated VA reclamation via thread_registry.
add_object_library(
  va_substrate
  SRCS
    alloc/legacy/va_substrate.cpp
  HDRS
    alloc/legacy/va_substrate.h
  DEPENDS
    .alloc_canary_seed
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .alloc_substrate_registry
    .bcryptprimitives
    .libc_fini_registry
    .mapping_table
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .page_alloc
    .page_size
    .process_control_block
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.__support.threads.windows.spin_wait
    libc.src.__support.threads.windows.thread_registry
)

# Layer 2 flat 8 B / 64 KiB chunkmap. Out-of-band address->metadata
# index for libc-allocator chunks, sealed Tier-A libc-internal ranges,
# and cordons (Image / Kernel / Foreign). POSIX-visible VA is NOT
# stamped here — it is tracked by `memory/va_tracker.h` (ART + interval
# skiplist) and the classifier composes the two indexes. Single 8 B
# cookie-XOR'd tagged-pointer entry encoding (slot_idx << 8 | tag).
# Stay-committed PAGE_READONLY backing array (snmalloc lazy-commit /
# notify_using_readonly pattern; Liétar et al ISMM 2019) — every
# load is wait-free and non-faulting from any context (SIGSEGV
# classifier, debug probes, fault classifier). Touched OS pages
# upgraded RO->RW lazily on first publish via nt_pal::protect.
# The chunk-VA decommit lives in the chunk-owning descriptor's
# Crystalline domain FreeFn (the §7.2 layered reclamation rule via
# FreeFn ordering), not in pagemap itself.
add_object_library(
  pagemap
  SRCS
    alloc/pagemap.cpp
    alloc/pagemap_cordon.cpp
  HDRS
    alloc/pagemap.h
    alloc/pagemap_cordon.h
  DEPENDS
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .process_control_block
    .section_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Sealed-VA publisher — single Tier-A chokepoint that every libc-sealed
# VA range routes through. Enforces disjointness between the pagemap,
# descriptor pools, and individual partition VAs at publication time
# (trap on overlap). NT's user-space VAD allocator packs hint-less
# reservations adjacently and the current reservation surface does not
# carry a base hint, so no inter-range gap is enforced today.
# Drives eager pagemap registration (tag=`LibcSealed`, slot_idx=`SealedKind`)
# for descriptor-pool-class ranges so wild pointers landing in libc-internal
# VA decode to a recognizable kind via the pagemap reader. POSIX-visible VA
# is out of scope here — that lookup goes through `memory/va_tracker.h`.
# Backing table is BSS-resident during Tier A and wiped at
# `seal_time_assert_and_wipe()` immediately before `pcb_seal_readonly_a`,
# so no centralized base-table honeypot persists post-seal.
add_object_library(
  alloc_sealed_va_publisher
  SRCS
    alloc/sealed_va_publisher.cpp
  HDRS
    alloc/sealed_va_publisher.h
  DEPENDS
    .pagemap
    .process_control_block
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Layer 2 NBALLOC lock-free buddy chunk broker. Reserves per-process
# 4 GiB compact-pointer partitions, hands out power-of-2 chunks
# (64 KiB - 4 MiB) via per-arena NBALLOC trees. Cross-class coalescing
# real (Marotta arXiv:1804.03436 v2). Pagemap-coupled (every chunk
# stamped via pagemap_store(slot_idx, tag) on alloc; decoded on free
# via pagemap_load_decoded; entries zeroed via pagemap_retire_range).
# Crystalline-W gated reclaim through g_arena_domain — FreeFn ordering
# is canary --> pagemap_unregister_range (no-op post-pivot) -->
# nt_pal::decommit_preserve --> return descriptor slot (no-leak /
# no-UAF guarantee, design doc S 7.2 layered reclamation rule). Hybrid
# init: eager VA reservation, lazy tree commit on first allocation.
# Sized-free API mandatory. Lock-free hot path, no spinlocks anywhere
# in the buddy itself.
add_object_library(
  buddy_arena
  SRCS
    alloc/buddy_arena.cpp
  HDRS
    alloc/buddy_arena.h
  DEPENDS
    .alloc_canary_seed
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .alloc_sealed_va_publisher
    .crystalline_domain
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .pagemap
    .process_control_block
    .section_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Layer 3 prerequisite — sub-slab decommit substrate. Owns the
# `committed_subpage_mask` (Atomic<uint16_t>, 1 bit per 4 KiB OS sub-page
# within a 64 KiB slab page) and the protocol contract that future
# slab_page.cpp / purge_timer.cpp will compose with. Pure-arithmetic /
# pure-atomic, no syscall surface — see header prologue for the
# RELEASE-clear-before-decommit / RELEASE-set-after-commit ordering rules
# and §9.3.10.1 sub-slab decommit machinery.
add_header_library(
  alloc_slab_chunk_state
  HDRS
    alloc/slab_chunk_state.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# ---- Layer 7 partition layer (`alloc::partition`) ---------------------------
#
# Type-isolated 4 GiB VA partitions enforced by VA disjointness across
# (class, numa_node) pairs. Owns the 4 GiB compact-pointer windows that
# every allocator subsystem draws from (buddy_arena chunks, future
# segment / slab_page / IndexedPool / ThreadScratch consumers). Provides
# the §8.4 source-of-truth for "is this VA allocator-owned?" — Layer 1's
# future va_tracker calls partition::lookup(addr) to skip allocator-
# internal VA on every fault.
#
# Three indices, three query modes, one descriptor pool:
#   * Coarse pagemap (snmalloc-style, 4 GiB stride flat index, 256 KiB
#     reservation lazy-committed) — addr→PartitionDescriptor* hot path,
#     ~3 ns warm.
#   * Reserve table (lock-free open-addressed hash on packed (cls, node))
#     — (cls, node)→PartitionDescriptor* deduplication for reserve_or_grow,
#     CAS-based, bounded retries.
#   * Pagemap consumer_tag stamping — chunk-owner-side, populates the
#     existing 64 KiB chunk pagemap entry's tag byte with partition
#     class for diagnostics + chunk-level fast classification.
#
# Reclamation is event-driven on the empty-transition (no polling, no
# heartbeat counter, no periodic sweep). Crystalline-W via
# g_partition_domain (slot 3 of 8-domain budget) gates the FreeFn until
# all in-flight lookup() pin holders release; FreeFn issues MEM_RELEASE
# on the 4 GiB VA.
add_header_library(
  alloc_partition_class
  HDRS
    alloc/partition_class.h
  DEPENDS
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_object_library(
  partition
  SRCS
    alloc/partition.cpp
  HDRS
    alloc/partition.h
  DEPENDS
    .alloc_canary_seed
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .alloc_partition_class
    .alloc_sealed_va_publisher
    .crystalline_domain
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .pagemap
    .process_control_block
    .section_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# va_tracker outer ART (P1.G L3) — ROWEX-synchronised Adaptive Radix Tree
# from Leis 2013 + Leis 2016. Per-node-type chunk-bitmap allocator within
# the PINNED PartitionClass::ArtNode partition; Crystalline-W
# `g_va_tracker_art_domain` (slot 4 of 8) gates retire of obsolete nodes.
#
# Implementation split (mirrors reference/repos/ARTSynchronized/ROWEX/):
#   art_node.{h,cpp}       — node types + lock primitives + insert/remove
#                             dispatch (= reference N.{h,cpp} + N{4..256}.cpp)
#   art_node_alloc.{h,cpp}  — chunk-bitmap allocator + Crystalline FreeFn
#   art_tree.cpp           — Tree class + lookup/insert/remove/walk_range
#                             (= reference Tree.cpp)
#   art_index.h            — public façade re-exporting the Tree API
add_object_library(
  art_index
  SRCS
    memory/art_node.cpp
    memory/art_node_alloc.cpp
    memory/art_tree.cpp
  HDRS
    memory/art_index.h
    memory/art_node.h
    memory/art_node_alloc.h
  DEPENDS
    .alloc_canary_seed
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .alloc_partition_class
    .va_tracker_chunk
    .crystalline_domain
    .nt_pal
    .ntdll
    .partition
    .process_control_block
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Header-only carrier for the SkiplistLinkTraits state-machine policy
# class consumed by the va_tracker interval skiplist. Pure substrate-Link
# adapter; no .cpp.
add_header_library(
  skiplist_link_traits
  HDRS
    memory/skiplist_link_traits.h
  DEPENDS
    .lock_free_linkage
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# va_tracker leaf RegionDesc payload — Crystalline-W-managed; cache-line.
# The header surface stays a leaf — `va_region_desc.cpp` (allocator,
# release, clone-for-fragment, fork-reinit phase) is compiled into the
# `interval_skiplist` object library, because it depends on the bucket
# capacity constants in `interval_skiplist.h` and the skiplist
# Crystalline domain instance — a separate library would create a cycle.
add_header_library(
  va_region_desc
  HDRS
    memory/va_region_desc.h
  DEPENDS
    .crystalline_domain
    .nt_types
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# va_tracker shared chunk-pool allocator. Process-global VaChunkDesc pool
# + chunk-state state-machine + commit_new_va_chunk_for + canary derivation.
# Consumed by both interval_skiplist (skiplist node chunks + RegionDesc
# chunks) and art_index (ART node chunks) — same partition_secret-based
# canary derivation, same Crystalline-W FreeFn discipline, single
# definition site for `va_chunk_desc_*` / `commit_new_va_chunk_for` /
# `va_chunk_init` / `va_chunk_fork_reinit`.
add_object_library(
  va_tracker_chunk
  SRCS
    memory/va_tracker_chunk.cpp
  HDRS
    memory/va_tracker_chunk.h
    memory/va_tracker_chunk_state.h
  DEPENDS
    .alloc_occupancy_bitmap
    .alloc_partition_class
    .crystalline_domain
    .nt_pal
    .ntdll
    .pagemap
    .partition
    .process_control_block
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# va_tracker per-leaf range index. Lock-free reader, lock-free writer
# with per-link state-byte LOCKED gate, multi-level Swap with single-CAS
# linearisation at level 0.
add_object_library(
  interval_skiplist
  SRCS
    memory/interval_skiplist.cpp
    memory/arena_alloc.cpp
    memory/va_region_desc.cpp
  HDRS
    memory/interval_skiplist.h
    memory/arena_alloc.h
    memory/chunked_append_store.h
  DEPENDS
    .alloc_occupancy_bitmap
    .alloc_partition_class
    .va_tracker_chunk
    .crystalline_domain
    .lock_free_linkage
    .nt_pal
    .ntdll
    .partition
    .process_control_block
    .skiplist_link_traits
    .va_region_desc
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.__support.threads.windows.futex_addr
)

# va_tracker (P1.G L4) — public Phase 1 VA-index API. Composes the outer
# ART (`art_index`) with the inner concurrent interval skiplist
# (`interval_skiplist`) into the surface defined by
# `NTPOSIX_MEMORY_ARCHITECTURE_DESIGN.md` §8.2: acquire / release /
# replace / mutate / split / resolve / walk_range / serialize_for_fork /
# replay_in_child / va_tracker_fork_reinit. Registers the Phase 6
# memory-primitive bootstrap entry and the `kForkPrioVaTracker = 39`
# fork-reinit hook.
#
# `va_tracker.cpp` carries the read-side machinery (`resolve`,
# `walk_range`, fork serialize/replay, init). `va_tracker_transaction.cpp`
# carries the write-side engine (the typed ops, the single-envelope
# commit machinery, `backing_kill_and_retire`). Both are part of the
# same logical module and share the `va_tracker.h` public surface — no
# intermediate `transaction.h` / `acquire.h` exists.
add_object_library(
  va_tracker
  SRCS
    memory/va_tracker.cpp
    memory/va_tracker_transaction.cpp
  HDRS
    memory/va_tracker.h
  DEPENDS
    .alloc_partition_class
    .art_index
    .crystalline_domain
    .desc_backing
    .interval_skiplist
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .page_alloc
    .pagemap
    .partition
    .section_registry
    .va_region_desc
    .va_tracker_chunk
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.error_or
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# desc_backing — the Crystalline-W-managed shared kernel-state object
# holding placeholder identity + section/file handles. Synchronous
# Stage 2 kernel-state teardown lives in `va_tracker_transaction.cpp`
# (`backing_kill_and_retire` body); Crystalline grace governs Stage 3
# slot reclamation (the FreeFn in this TU is metadata-only).
add_object_library(
  desc_backing
  SRCS
    memory/desc_backing.cpp
  HDRS
    memory/desc_backing.h
  DEPENDS
    .alloc_partition_class
    .crystalline_domain
    .ntdll
    .pagemap
    .partition
    .process_control_block
    .va_tracker_chunk
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# ============================================================================
# POSIX memory-op layer (Layer 8).
#
# Header utilities and DescMutator callbacks consumed by the per-op
# files under `memory/posix/{mmap,madvise,mremap,...}/`. Header-only
# except for `posix_mutators.cpp`, which carries the twelve free-function
# callbacks the substrate's `va_tracker::mutate` invokes.
# ============================================================================

# POSIX-layer errno overrides for the three call sites where POSIX
# mandates an errno different from the generic `nt_error.h` mapping
# (MAP_FIXED_NOREPLACE → EEXIST, MAP_HUGETLB → EPERM, mlock retry →
# EAGAIN), plus an `ErrorOr<T>` → negative-errno helper.
add_header_library(
  memory_posix_errno
  HDRS
    memory/posix/posix_errno.h
  DEPENDS
    .ntdll
    libc.hdr.errno_macros
    libc.src.__support.error_or
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Page-rounding, overflow-safe size checks, POSIX prot/flag validation,
# HUGETLB privilege probe, MAP_HUGE_SHIFT decoder, madvise advice-code
# whitelist. Pure validation — no syscalls, no substrate touch.
add_header_library(
  memory_posix_validation
  HDRS
    memory/posix/posix_validation.h
  DEPENDS
    .alloc_page_size
    .nt_pal
    .ntdll
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# AcquireMeta builders, one per POSIX call shape (anon_private,
# anon_shared, file_private, file_shared, hugetlb, brk, shm_posix,
# shm_sysv). The single site that translates POSIX flag bits into
# `region_flag::*` bits the substrate's fault and fork paths consume.
add_header_library(
  memory_posix_meta
  HDRS
    memory/posix/posix_meta.h
  DEPENDS
    .memory_posix_validation
    .nt_pal
    .ntdll
    .va_region_desc
    .va_tracker
    libc.hdr.stdint_proxy
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Twelve `DescMutator` free-function callbacks for the POSIX layer.
# Each mutator runs on a fresh `RegionDesc` clone under the substrate's
# LOCKED hold and writes only the fields its intent names; unrelated
# `flags` bits are preserved through the OR / AND-NOT pattern in
# `set_flag_bits` / `clear_flag_bits`.
add_object_library(
  memory_posix_mutators
  SRCS
    memory/posix/posix_mutators.cpp
  HDRS
    memory/posix/posix_mutators.h
  DEPENDS
    .ntdll
    .va_region_desc
    .va_tracker
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Reserve-and-commit atexit callback list. Replaces BlockStore on Windows
# to avoid the malloc → atexit circular dependency.
add_header_library(
  exit_callbacks
  HDRS
    process/exit_callbacks.h
  DEPENDS
    .ntdll
    libc.src.__support.common
)

# Basic Windows types (HANDLE, NTSTATUS, LARGE_INTEGER, etc.)
add_header_library(
  nt_types
  HDRS
    nt/nt_types.h
  DEPENDS
    libc.src.__support.CPP.limits
    libc.src.__support.macros.config
)

# PEB / TEB layout definitions.
add_header_library(
  nt_peb
  HDRS
    nt/nt_peb.h
  DEPENDS
    .nt_types
)

# WCHAR string_view over the NT UNICODE_STRING ABI.
add_header_library(
  nt_wstring_view
  HDRS
    nt/nt_wstring_view.h
  DEPENDS
    libc.src.__support.CPP.limits
    libc.src.__support.common
    libc.src.__support.macros.config
)

# NT security, token, and registry type declarations.
add_header_library(
  nt_security_types
  HDRS
    nt/nt_security_types.h
  DEPENDS
    .nt_types
)

# NT security, token, and registry API declarations.
add_header_library(
  nt_security_api
  HDRS
    nt/nt_security_api.h
  DEPENDS
    .nt_security_types
)

# Umbrella header for NT security declarations.
add_header_library(
  nt_security
  HDRS
    nt/nt_security.h
  DEPENDS
    .nt_security_types
    .nt_security_api
)

# NT process and thread type declarations.
add_header_library(
  nt_process_types
  HDRS
    nt/nt_process_types.h
  DEPENDS
    .nt_peb
    .nt_security_types
)

# NT process and thread API declarations.
add_header_library(
  nt_process_api
  HDRS
    nt/nt_process_api.h
  DEPENDS
    .nt_process_types
)

# Umbrella header for NT process declarations.
add_header_library(
  nt_process
  HDRS
    nt/nt_process.h
  DEPENDS
    .nt_process_types
    .nt_process_api
)

# OBJECT_ATTRIBUTES helpers with explicit inheritance control.
add_header_library(
  handle_attributes
  HDRS
    nt/handle_attributes.h
  DEPENDS
    .nt_types
    .nt_wstring_view
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Lightweight RAII wrapper for NT kernel handles.
add_header_library(
  scoped_nt_handle
  HDRS
    nt/scoped_nt_handle.h
  DEPENDS
    .nt_process_api
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# NT build/capability probing and optional-syscall dispatch table.
add_object_library(
  nt_capabilities
  HDRS
    nt/nt_capabilities.h
  SRCS
    nt/nt_capabilities.cpp
  DEPENDS
    .nt_types
    .ntdll
    .process_control_block
    libc.src.__support.macros.config
)

# AFD (Ancillary Function Driver) socket IOCTL codes and structures.
add_header_library(
  nt_afd
  HDRS
    nt/nt_afd.h
  DEPENDS
    .nt_types
)

# NT file types (IO_STATUS_BLOCK, FILE_IO_COMPLETION_INFORMATION, etc.)
add_header_library(
  nt_file
  HDRS
    nt/nt_file.h
  DEPENDS
    .nt_types
)

# NT IoRing types and syscall declarations.
add_header_library(
  nt_ioring
  HDRS
    nt/nt_ioring.h
  DEPENDS
    .nt_types
)

# NT-native IoRing operations (replaces kernelbase.dll wrappers).
# SQE push / CQE pop are pure userspace; only submit() is a syscall.
add_header_library(
  nt_ioring_ops
  HDRS
    nt/nt_ioring_ops.h
  DEPENDS
    .nt_ioring
    libc.src.__support.macros.attributes
)

# NT IPC / ALPC types and syscall declarations.
# Used by the signal subsystem's cross-process ALPC transport.
add_header_library(
  nt_ipc
  HDRS
    nt/nt_ipc.h
    nt/nt_ipc_types.h
    nt/nt_ipc_api.h
  DEPENDS
    .nt_types
    libc.src.__support.macros.config
)

# Process-wide IOCP reactor — foundational event backbone for NT-POSIX.
# Single IOCP + drain thread that multiplexes ALPC signals, process waits,
# timers, and any waitable NT handle. Subsystems register via watch/watch_alpc.
# Reactor: single object library combining the header and implementation.
# Consumers that depend on .reactor get both the declarations and the compiled
# object, so collect_object_file_deps always pulls reactor.cpp into the link.
add_object_library(
  reactor
  HDRS
    reactor/reactor.h
  SRCS
    reactor/reactor.cpp
  DEPENDS
    .alloc_guarded_region
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .libc_fini_registry
    .nt_types
    .nt_file
    .nt_ipc
    .ntdll
    .page_alloc
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
)

# Shared multiplexer atop reactor::set_completion_router. Allows epoll
# and socket I/O (and future subsystems) to coexist on the single
# reactor IOCP — each registers either a key-sentinel handler or the
# default catch-all handler.
add_object_library(
  completion_router
  HDRS
    reactor/completion_router.h
  SRCS
    reactor/completion_router.cpp
  DEPENDS
    .nt_types
    .reactor
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

# NT path conversion utilities (shared by FILE* and fd layers)
add_header_library(
  nt_path
  HDRS
    nt/path_resolver.h
    nt/nt_path_convert.h
    nt/nt_path.h
    nt/session_bno.h
  DEPENDS
    .ntdll
    .fd_table
    .temp_path
    .process_control_block
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.macros.attributes
)

add_header_library(
  temp_path
  HDRS
    temp_path.h
  DEPENDS
    .ntdll
    libc.hdr.types.size_t
    libc.src.__support.macros.attributes
)

add_object_library(
  condrv
  HDRS
    ipc/condrv.h
    ipc/condrv_types.h
    ipc/condrv_ops.h
  SRCS
    ipc/condrv_ops.cpp
  DEPENDS
    .nt_file
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# IO Ring shared helpers (submit/wait, open-with-ring, cqe_status_to_errno)
add_header_library(
  io_ring_helpers
  HDRS
    io/io_ring_helpers.h
  DEPENDS
    .ntdll
    .nt_ioring_ops
    libc.hdr.errno_macros
    libc.src.__support.File.file  # for FileIOResult (via file_io_result.h)
    libc.src.__support.macros.attributes
)

# FIFO support (mkfifo marker files + NT named pipe open state machine)
add_header_library(
  fifo
  HDRS
    ipc/fifo.h
  DEPENDS
    .fd_table
    .ntdll
    .nt_path
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.mode_t
    libc.src.__support.error_or
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# FIFO shared-memory ring buffer channel (cross-process data transfer)
add_header_library(
  fifo_channel
  HDRS
    ipc/fifo_channel.h
  DEPENDS
    .ntdll
    .ntdll
    .page_alloc
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.signal_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Socket AFD I/O completion — zero-event IOCP dispatch + ThreadLocalWord
# parking. Shared by every AFD ioctl caller through afd_core helpers.
add_object_library(
  afd_io_completion
  HDRS
    ipc/afd_io_completion.h
  SRCS
    ipc/afd_io_completion.cpp
  DEPENDS
    .completion_router
    .nt_afd
    .nt_types
    .ntdll
    .reactor
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.thread_lifecycle
    libc.src.__support.threads.windows.thread_local_word
)

# Socket state (SocketPhase, SocketState, pool alloc/free)
add_header_library(
  socket_state
  HDRS
    ipc/socket_state.h
  DEPENDS
    .afd_io_completion
    .nt_types
    .page_alloc
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Socketpair channel (dual ring buffer cross-wiring)
add_header_library(
  sockpair_channel
  HDRS
    ipc/sockpair_channel.h
  DEPENDS
    .fifo_channel
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Generic AFD plumbing (afd_ioctl, afd_open_endpoint, ntstatus_to_errno_socket)
add_header_library(
  afd_core
  HDRS
    ipc/afd_core.h
  DEPENDS
    .afd_io_completion
    .nt_afd
    .ntdll
    .socket_state
    libc.hdr.errno_macros
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.thread_lifecycle
)

# Address family operations dispatch table
add_header_library(
  af_ops
  HDRS
    ipc/af_ops.h
  DEPENDS
    .nt_types
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# AF_UNIX operations (Work structs and constants)
add_header_library(
  af_unix_ops_hdrs
  HDRS
    ipc/af_unix_ops.h
  DEPENDS
    .af_ops
    .socket_state
    .nt_afd
    libc.src.__support.macros.config
)

# AF_UNIX operations implementation
add_object_library(
  af_unix_ops
  SRCS
    ipc/af_unix_ops.cpp
  HDRS
    ipc/af_unix_ops.h
  DEPENDS
    .af_ops
    .afd_core
    .afd_io_completion
    .socket_state
    .nt_afd
    libc.hdr.errno_macros
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.hdr.types.struct_sockaddr_un
    libc.include.llvm-libc-macros.sys_socket_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.thread_lifecycle
)

# Address family ops resolver — central registry, owns resolve_af_ops().
# Adding a new AF means: create af_<name>_ops.{h,cpp}, add a case here.
add_object_library(
  af_resolve
  SRCS
    ipc/af_resolve.cpp
  DEPENDS
    .af_ops
    .af_unix_ops
    libc.include.llvm-libc-macros.sys_socket_macros
    libc.src.__support.macros.config
)

# Epoll state (EpollInstance, EpollRegistration, hash table)
add_header_library(
  socket_epoll
  HDRS
    ipc/socket_epoll.h
  DEPENDS
    .nt_types
    .page_alloc
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.raw_mutex
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Syscall number definitions — standalone, zero dependencies.
# Includable without pulling in dispatch machinery or transitive deps.
add_header_library(
  syscall_numbers
  HDRS
    syscall_numbers.h
)

# Syscall compatibility header — POSIX syscall personality dispatch.
# Provides syscall_impl overloads that route Linux SYS_* numbers to
# Windows userspace engines. Zero-cost dispatch via constant folding.
#
# Inlining discipline:
#   - syscall_impl / syscall_dispatch: [[gnu::always_inline]] (non-negotiable
#     for constant folding — as the switch grows, compiler heuristics become
#     LESS likely to inline it; always_inline prevents this regression)
#   - Trivial targets (TEB reads, atomic loads): [[gnu::always_inline]]
#   - Complex targets (mmap, kill, sigprocmask, I/O): forward-declared,
#     out-of-line — resolved at link time
#
# CMake dependencies only for targets that don't create cycles:
#   - signal_subsystem: SYS_tgkill, SYS_kill, SYS_rt_sigprocmask, etc.
#   - process_identity: SYS_getuid, SYS_geteuid, SYS_getgid, SYS_getegid
#   - kernel32/ntdll: SYS_getpid, SYS_gettid, SYS_sched_yield, SYS_exit
#
# Targets that WOULD create cycles (fd_table, mapping_table, clock) are
# forward-declared only — no CMake dep. The linker resolves them from
# the implementation's object library at final link.
add_header_library(
  syscall
  HDRS
    syscall.h
  DEPENDS
    .ntdll
    .process_control_block
    .syscall_numbers
    libc.hdr.errno_macros
    libc.hdr.types.mode_t
    libc.hdr.types.off_t
    libc.hdr.types.pid_t
    libc.hdr.types.size_t
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.bit
    libc.src.__support.common
    libc.src.__support.macros.config
)

# Aggregation of every subsystem implementing a target of the `syscall_impl`
# switch: every out-of-line internal::* helper (io, memory, time, ipc,
# resource) and every signal_state::* helper invoked from syscall.h. Public
# syscall entrypoints (libc.src.unistd.{syscall,__llvm_libc_syscall}) depend
# on this so their link closure is self-sufficient in freestanding-unit test
# builds — where no c.dll is present to provide the full NT personality.
# Kept out of `.syscall` itself to preserve the forward-declaration-only
# discipline that breaks fd_table / mapping_table / clock dependency cycles.
add_header_library(
  syscall_dispatch_targets
  HDRS
    syscall_dispatch_targets.h
  DEPENDS
    .brk_state
    .clock_ops
    .fcntl
    .inotify_ops
    .mmap_engine
    .priority_ops
    .read_write
    .signal_syscalls
    .vm_protect
    # Mass-wire additions: every engine reachable from syscall_dispatch.
    # File I/O
    .file_ops
    .stat_ops
    .path_ops
    .mkdir_ops
    .rename_ops
    .scatter_io_ops
    .utimes_ops
    # Permissions
    .chmod_ops
    .ownership_ops
    # Memory
    .mremap_engine
    .madvise_ops
    .mincore_ops
    .msync_ops
    .mlock_ops
    .remap_file_pages_ops
    .numa_ops
    .pkey_ops
    .shm_ops
    .sysv_shm_ops
    # Process / identity / resource
    .identity_ops
    .wait_ops
    .exec_ops
    .resource_ops
    # fork engine — naked-asm clone orchestrator, reached via
    # __llvm_libc_sys_fork at link time.
    libc.src.unistd.fork
    # Sockets / multiplexing / SysV sem / cross-fd
    .socket_lifecycle
    .socket_data
    .socket_query
    .poll_ops
    .select_ops
    .epoll_ops
    .sysv_sem_ops
    .sendfile_ops
    # Scheduling
    .sched_ops
    .affinity_ops
    .scheduler_ops
    # Time
    .itimer_ops
    .timer_ops
    libc.src.__support.time.windows.clock_nanosleep
    libc.src.__support.time.windows.clock_settime
    libc.src.__support.time.windows.nanosleep
    # Futex (SYS_futex → futex_addr::wait/wake)
    libc.src.__support.threads.windows.futex_utils
)

# Page size utilities (cached system page info).
add_header_library(
  page_size
  HDRS
    alloc/page_size.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.config
)

# POSIX file permission helpers (chmod/chown/umask + stat mode reconstruction)
add_header_library(
  security
  HDRS
    security/security.h
  DEPENDS
    .ntdll
    libc.src.__support.CPP.atomic
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# LSA client API declarations for S4U logon (setuid/seteuid)
add_header_library(
  sspicli_header
  HDRS
    security/sspicli.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.config
)

# DSE-proof, cache-line-evicting zero wipe for secret material.
# Use on buffers containing authentication tokens, key bytes, or any
# data where a post-wipe transient-execution peek would be a compromise.
add_header_library(
  secure_zero
  HDRS
    security/secure_zero.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.hdr.types.size_t
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.string.memory_utils.inline_memset
)

# Pre-fork quiescence + post-fork resume strong impls.
#
# Lives in its own object library because fork_reinit.cpp carries
# matching `[[gnu::weak]]` defaults for libc_fork_quiesce / libc_fork_resume
# (which __builtin_trap), and a single TU cannot define both weak and
# strong forms of the same symbol. The strong impls have no externally-
# referenced symbols of their own — only the strong overrides — so static-
# archive linkers (libc.lib in hermetic test EXEs) drop the .obj unless
# something else pulls it in. The matching LIBC_ANCHOR_DEFINE(fork, quiesce)
# in fork_quiesce.cpp + LIBC_FORCE_PULL_GLOB("__libc_anchor_fork_*") in
# fork_reinit.cpp provide that pull at link time.
add_object_library(
  fork_quiesce
  SRCS
    process/fork_quiesce.cpp
  HDRS
    libc_subsystem_init.h
  DEPENDS
    .ntdll
    .section_registry
    libc.src.__support.File.file
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.__support.macros.config
)

# Fork-child reinitialization — delegates to explicit libc_fork_reinit().
#
# DEPENDS pulls in BOTH strong-impl object libraries (fork_quiesce +
# libc_fork_reinit_impl) so the per-test trimmed libc archive includes
# their .obj files. The LIBC_FORCE_PULL_GLOB in fork_reinit.cpp then
# matches the LIBC_ANCHOR_DEFINE(fork, ...) symbols in those TUs and
# forces selection at link time. Without the DEPENDS edges, archive
# trimming would not include them in the first place.
add_object_library(
  fork_reinit
  SRCS
    process/fork_reinit.cpp
  HDRS
    process/fork_reinit.h
    libc_subsystem_init.h
  DEPENDS
    .fork_quiesce
    .libc_fork_reinit_impl
    .ntdll
    .section_registry
    windows_ntdll
    libc.src.__support.macros.config
    libc.src.__support.threads.fork_callbacks
)

# Lock-free intrusive linkage substrate — header-only. Provides the
# `linkage::Link` packed atomic word and the `link_cas_*` family used
# by wait_slot, futex_utils, futex_addr, and any future consumer
# wanting a hand-rolled lock-free chain. The Safety Triad documented
# at the top of `lock_free_linkage.h` (tag monotonicity +
# IDLE-on-reachable-chain retry + never-freed pool memory) replaces
# hazard pointers / retire queues for every chain built on it.
add_header_library(
  lock_free_linkage
  HDRS
    concurrent/lock_free_linkage.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Crystalline per-thread POD state — leaf header library. Broken out so
# thread_scratch.h can embed the per-domain `crystalline_slot_idx[]` and
# `crystalline_batches[]` arrays on the per-thread control block without
# pulling in the full CrystallineDomain template, and so
# crystalline_domain.h can depend on thread_scratch.h for
# get_thread_scratch() without a cycle. Also hosts the slot pool header
# (crystalline_slot_pool.h) — its lifetime constraints (process-lifetime
# VA, demand-committed, never freed) match the local_state contract.
add_header_library(
  crystalline_local_state
  HDRS
    concurrent/crystalline_local_state.h
    concurrent/crystalline_slot_pool.h
  DEPENDS
    .lock_free_linkage
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Crystalline registry state — non-template, compiled once. Provides
# registry_push / registry_fork_reinit_all / registry_fini_all /
# registry_flush_thread_all + the per-domain cookie helper. Does NOT
# pull in thread_scratch, so thread_scratch.cpp can depend on it (it
# calls registry_flush_thread_all from scratch_thread_cleanup via
# scratch_flush_crystalline_batches) without creating a CMake cycle
# with the template-side crystalline_domain library that does depend
# on thread_scratch.
add_object_library(
  crystalline_registry
  SRCS
    concurrent/crystalline_domain.cpp
  HDRS
    concurrent/crystalline_domain_registry.h
  DEPENDS
    .crystalline_local_state
    .libc_fini_registry
    .process_control_block
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# CrystallineDomain<> template — header-only façade over crystalline_registry.
# Consumers of the SMR primitive include crystalline_domain.h and get
# both the template machinery and (transitively) the registry symbols.
# Pulls in thread_scratch because the template's per-thread accessors
# reach the arena-resident CrystallineThreadRegion through
# ThreadScratchState::crystalline_region(); cross-thread helping walks
# scratch_detail::g_scratch_head.
add_header_library(
  crystalline_domain
  HDRS
    concurrent/crystalline_domain.h
  DEPENDS
    .crystalline_local_state
    .crystalline_registry
    .thread_scratch
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.type_traits
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Explicit ordered subsystem init/fini/fork-reinit for c.dll (no section dispatch).
#
# Split into three object libraries so that freestanding consumers (the libc
# test-suite standalone binaries that call __libc_init which in turn calls
# __libc_dll_init) only pay for the init path, not the fork-child and
# shutdown paths that reach into every fork_reinit / fini symbol.
add_object_library(
  libc_subsystem_init
  SRCS
    libc_subsystem_init.cpp
  HDRS
    libc_subsystem_init.h
  DEPENDS
    # Phase 3 — allocator
    libc.src.stdlib.windows.posix_alloc
    # Phase 4 — pools
    libc.src.__support.threads.windows.thread_lifecycle
    .ofd_pool
    .file_pool
    libc.src.__support.threads.windows.wait_slot
    libc.src.__support.threads.windows.mutex
    .thread_ring
    libc.src.__support.threads.windows.thread
    .brk_state
    # Phase 6 — fd table
    .fd_table
    # Phase 7 — services
    .reactor
    .afd_io_completion
    .signal_subsystem
    .fcntl_lock_table
    .alpc_bus
    # Misc collaborators
    .lazy_init
    .libc_fini_registry
    .ntdll
    .veh_filter_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.process_control_block
)

add_object_library(
  libc_fork_reinit_impl
  SRCS
    libc_fork_reinit_impl.cpp
  HDRS
    libc_subsystem_init.h
  DEPENDS
    .bcryptprimitives
    .ntdll
    .section_registry
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.process_control_block
)

add_object_library(
  libc_subsystem_fini
  SRCS
    libc_subsystem_fini.cpp
  HDRS
    libc_subsystem_init.h
  DEPENDS
    .libc_fini_registry
    .ntdll
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.process_control_block
)

# VEH reentry guard — single TEB TLS slot preventing infinite recursion.
# Must initialize before veh_core_startup_init() (Phase 1).
add_object_library(
  veh_reentry_guard
  SRCS
    veh/veh_reentry_guard.cpp
  HDRS
    veh/veh_reentry_guard.h
  DEPENDS
    .ntdll
    .teb_tls
    libc.src.__support.macros.config
)

# VEH core — single master handler with priority-sorted dispatch table.
# veh_core_startup_init() (Phase 1). All subsystems register filters into this.
add_object_library(
  veh_core
  SRCS
    veh/veh_core.cpp
  HDRS
    veh/veh_core.h
  DEPENDS
    .fork_reinit
    .lazy_init_reset
    .libc_fini_registry
    .module_block
    .ntdll
    .r_debug
    .section_registry
    .veh_reentry_guard
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.setjmp.setjmp
)

# `.libcveh` SectionRegistry — declarative registration for static VEH
# filters. Owner of the $A/$Z bookends + the Tier B Phase 3 sweep walker.
# Filter-owning TUs depend on the *header* (for the macro); only this TU
# defines the bookends and provides register_all_static_veh_filters().
add_object_library(
  veh_filter_registry
  SRCS
    veh/veh_filter_registry.cpp
  HDRS
    veh/veh_filter_registry.h
  DEPENDS
    .process_control_block
    .section_registry
    .veh_core
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
)

# RegionPool — descriptor allocator for logical mmap regions. Backs every
# slot in the mapping table with a refcounted RegionDesc carrying the
# section/file handles, shape, flags, and ChunkList for CHUNKED regions.
add_object_library(
  region_pool
  SRCS
    memory/legacy/region_pool.cpp
  HDRS
    memory/legacy/region_desc.h
    memory/legacy/region_pool.h
  DEPENDS
    .fork_reinit
    .ntdll
    .page_alloc
    .page_size
    .slab_pool
    libc.src.__support.CPP.atomic
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
)

# Mapping table + VA discovery.
#
# Two related pieces compiled as one library so they can reference
# each other freely (mapping_table.cpp invokes va_inventory_startup_
# discover(); va_inventory.h's DLL notification callback reaches into
# g_mapping_table):
#
#   * mapping_table.cpp — Phase 0c.5 startup, fork reinit, fini.
#                         Radix pages (L1 directory, L2Page, L3Page)
#                         are carved out of the VA substrate's sub-
#                         slot pools — L1 / L2 from class Small
#                         (16 KB), L3 from class Large (128 KB).
#                         Substrate seeds are stamped LIBC_INTERNAL
#                         by the walker's Pass 2; post-bootstrap
#                         substrate arenas self-register, so L2 / L3
#                         growth is always table-covered.
#   * va_inventory.h    — header-only discovery (TEB/PEB/stack, loaded
#                         modules, bulk-VAD sweep, DLL notifications).
add_object_library(
  mapping_table
  SRCS
    memory/legacy/mapping_table.cpp
  HDRS
    memory/legacy/mapping_table.h
    memory/va_inventory.h
  DEPENDS
    .fork_reinit
    .libc_fini_registry
    .memory_primitives_registry
    .nt_pal
    .ntdll
    .page_alloc
    .page_size
    .region_pool
    .va_substrate
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.futex_utils
    libc.src.__support.macros.config
)

# `.libcmem` section registry — handler header for memory-primitive
# subsystems (VaSubstrate, MappingTable, future per-CPU arenas, ...).
# Header-only leaf: every consumer registers via LIBC_REGISTER_MEMORY_
# PRIMITIVE from this header; the walker implementation lives in
# `memory_primitives_bootstrap` below and carries the link-time deps to
# the mapping table. Splitting the header out as a leaf breaks the
# otherwise-circular CMake graph mapping_table ↔ memory_primitives.
add_header_library(
  memory_primitives_registry
  HDRS
    memory/legacy/memory_primitives_bootstrap.h
  DEPENDS
    .section_registry
    libc.hdr.stdint_proxy
    libc.src.__support.macros.config
)

# `.libcmem` walker — Tier A Phase 0c.5 bootstrap.
# Owner of the $A/$Z bookends + the walker entry point used by
# __libc_bootstrap. Depends on mapping_table so Pass 2 can call
# register_mapping_internal on every Receipt accumulated by Pass 1.
add_object_library(
  memory_primitives_bootstrap
  SRCS
    memory/legacy/memory_primitives_bootstrap.cpp
  DEPENDS
    .mapping_table
    .memory_primitives_registry
    .process_control_block
    .section_registry
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
)

# Region reconciliation — bulk VA scan + table sync. Hooked into the
# fork / exec / dlopen event boundaries to keep the mapping table in
# agreement with NT reality after major VA-state changes.
add_object_library(
  region_reconcile
  SRCS
    memory/legacy/region_reconcile.cpp
  HDRS
    memory/legacy/region_reconcile.h
  DEPENDS
    .mapping_table
    .nt_pal
    .ntdll
    .page_alloc
    .page_size
    .region_pool
    .thread_scratch
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.spin_wait
)

# Memory fault VEH filter (remap guard + demand-commit). Registered into
# the unified VEH dispatch table at VEH_PRIORITY_MEMORY via a `.libcveh`
# record; the Tier B Phase 3 sweep (register_all_static_veh_filters())
# installs it after mmap_subsystem_init() completes.
add_object_library(
  mem_fault_handler
  SRCS
    memory/mem_fault_handler.cpp
  HDRS
    memory/mem_fault_handler.h
  DEPENDS
    .mapping_table
    .memory_lock_policy_state
    .nt_pal
    .ntdll
    .va_tracker
    .veh_core
    .veh_filter_registry
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

# RAII guard for the remap protocol (begin_remap/commit_remap/abort_remap
# + snapshot + COW + unmap + automatic rollback). Used by RemapTransaction
# (split-remap) and directly by NUMA rebind and remap_file_pages.
add_object_library(
  remap_guard
  SRCS
    memory/legacy/remap_guard.cpp
  HDRS
    memory/legacy/remap_guard.h
  DEPENDS
    .mapping_table
    .nt_pal
    .ntdll
    .page_alloc
    .page_size
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Transactional split-remap (carve a sub-range from a view, remap kept
# fragments). Built on RemapGuard. Used by mmap MAP_FIXED, partial munmap,
# mremap shrink.
add_object_library(
  remap_transaction
  SRCS
    memory/legacy/remap_transaction.cpp
  HDRS
    memory/legacy/remap_transaction.h
  DEPENDS
    .mapping_table
    .nt_pal
    .remap_guard
    .page_size
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# Process-wide POSIX identity state (uid/gid three-ID model)
add_object_library(
  process_identity
  SRCS
    process/process_identity.cpp
  HDRS
    process/process_identity.h
  DEPENDS
    .ntdll
    .process_control_block
    .secure_zero
    .sspicli_header
    windows_sspicli
    .fork_reinit
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

# Process creation utilities (shared between posix_spawn and exec*)
add_header_library(
  process_utils
  HDRS
    process/process_utils.h
  DEPENDS
    .ntdll
    .ntdll
    libc.hdr.errno_macros
    libc.src.__support.macros.config
    libc.src.string.string_utils
)

# SID <-> uid/gid conversion helpers shared by identity code.
add_header_library(
  sid_utils
  HDRS
    process/sid_utils.h
  DEPENDS
    .ntdll
    libc.hdr.types.gid_t
    libc.hdr.types.uid_t
    libc.src.__support.common
    libc.src.__support.macros.config
)

# posix_spawn engine — command line construction, env block, file actions,
# NtCreateUserProcess, child table registration.
add_object_library(
  spawn_ops
  SRCS
    process/spawn_ops.cpp
  HDRS
    process/spawn_ops.h
  DEPENDS
    .fcntl
    .fd_ops
    .fd_table
    .ntdll
    .ntdll
    .process_identity
    .process_utils
    .signal_subsystem
    libc.hdr.errno_macros
    libc.hdr.sched_macros
    libc.hdr.spawn_macros
    libc.include.spawn
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.process.windows.child_table
)

# ---------------------------------------------------------------------------
# Signal engine — four-layer POSIX signal architecture.
# Moved from src/signal/windows/ (Phase 2.1). Entry points remain there.
#
# Layer 1: Pending Storage (pending/) — lock-free pending sets + Vyukov MPSC
# Layer 2: Transports (transport/) — VEH, console, APC (intra + cross-process)
# Layer 3: Dispatch Engine (dispatch/) — drain, mask, invoke, SA_RESTART
# Layer 4: Process Control (control/) — stop/continue, SIGCHLD, default actions
# ---------------------------------------------------------------------------

# --- Layer 1: Pending Storage headers ---

add_header_library(
  signal_rt_queue_h
  HDRS
    signal/pending/rt_queue.h
  DEPENDS
    libc.hdr.signal_macros
    libc.hdr.types.pid_t
    libc.hdr.types.union_sigval
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_header_library(
  signal_pending_storage_h
  HDRS
    signal/pending/pending_storage.h
  DEPENDS
    .signal_constants_h
    .signal_rt_queue_h
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_header_library(
  signal_sigqueue_pool_h
  HDRS
    signal/pending/sigqueue_pool.h
  DEPENDS
    .signal_rt_queue_h
    libc.src.__support.macros.config
)

# --- Layer 3: Dispatch Engine headers ---

add_header_library(
  signal_restart_state_h
  HDRS
    signal/dispatch/restart_state.h
  DEPENDS
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_header_library(
  signal_dispatch_engine_h
  HDRS
    signal/dispatch/dispatch_engine.h
  DEPENDS
    .signal_pending_storage_h
    .signal_rt_queue_h
    libc.hdr.signal_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_header_library(
  signal_dispatch_fwd_h
  HDRS
    signal/dispatch/dispatch_fwd.h
  DEPENDS
    libc.src.__support.macros.config
)

add_header_library(
  signal_handler_table_h
  HDRS
    signal/dispatch/handler_table.h
  DEPENDS
    libc.hdr.types.struct_sigaction
    libc.src.__support.macros.config
)

add_header_library(
  signal_alt_stack_h
  HDRS
    signal/dispatch/alt_stack.h
  DEPENDS
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# --- Layer 4: Process Control headers ---

add_header_library(
  signal_process_control_h
  HDRS
    signal/control/process_control.h
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

add_header_library(
  signal_sigchld_h
  HDRS
    signal/control/sigchld.h
  DEPENDS
    libc.hdr.types.siginfo_t
    libc.src.__support.macros.config
)

# Layer 4 supporting subsystem: rich signal payload (Crystalline-backed).
# Wait-free per-signum atomic latest-event pointer; one Crystalline domain
# carries SIGCHLD / SIGEV_SIGNAL / cross-process kill records. Replaces
# the old PCB-resident packed SIGCHLD atomic and the racy CrossProcessSender
# side array with a single uniform mechanism that is signal-safe and
# NtTerminateThread-resilient.
add_header_library(
  signal_payload_h
  HDRS
    signal/payload/sig_payload.h
  DEPENDS
    libc.hdr.types.siginfo_t
    libc.hdr.types.union_sigval
    libc.src.__support.macros.config
)

add_object_library(
  signal_payload
  SRCS
    signal/payload/sig_payload.cpp
  DEPENDS
    .signal_payload_h
    .crystalline_domain
    libc.hdr.signal_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# --- Core signal type headers ---

add_header_library(
  signal_constants_h
  HDRS
    signal/signal_constants.h
  DEPENDS
    libc.hdr.signal_macros
    libc.hdr.stdint_proxy
    libc.hdr.types.sigset_t
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

add_header_library(
  signal_types_h
  HDRS
    signal/signal_types.h
  DEPENDS
    .signal_constants_h
    .signal_pending_storage_h
    .signal_restart_state_h
    libc.hdr.signal_macros
    libc.hdr.types.sigset_t
    libc.hdr.types.stack_t
    libc.src.__support.CPP.atomic
    .ntdll
)

add_header_library(
  signal_h
  HDRS
    signal/signal.h
  DEPENDS
    .signal_types_h
    libc.hdr.types.siginfo_t
    libc.hdr.types.ucontext_t
)

add_header_library(
  signal_internal_h
  HDRS
    signal/signal_internal.h
  DEPENDS
    .signal_h
    .signal_pending_storage_h
    libc.hdr.types.struct_sigaction
    libc.src.__support.threads.raw_mutex
)

add_header_library(
  syscall_frame_h
  HDRS
    signal/syscall_frame.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.config
)

# --- Layer 2: Transport headers ---

add_header_library(
  signal_veh_transport_h
  HDRS
    signal/transport/veh_transport.h
  DEPENDS
    .nt_types
    libc.src.__support.macros.config
)

add_header_library(
  signal_console_transport_h
  HDRS
    signal/transport/console_transport.h
  DEPENDS
    .nt_types
    libc.src.__support.macros.config
)

add_header_library(
  signal_apc_transport_h
  HDRS
    signal/transport/apc_transport.h
  DEPENDS
    .nt_types
    libc.hdr.types.pid_t
    libc.hdr.types.siginfo_t
    libc.src.__support.macros.config
)

add_header_library(
  signal_alpc_transport_h
  HDRS
    signal/transport/alpc_transport.h
  DEPENDS
    libc.hdr.types.pid_t
    libc.hdr.types.siginfo_t
    libc.src.__support.macros.config
)

# Generic ALPC bus: port lifecycle, opcode dispatch, request/reply.
# Consumed today by signal delivery and fcntl lock ownership attestation;
# any future cross-process query registers one opcode here.
add_header_library(
  alpc_bus_state_h
  HDRS
    ipc/alpc_bus_state.h
  DEPENDS
    .nt_ipc
    libc.src.__support.macros.config
)

add_header_library(
  alpc_bus_h
  HDRS
    ipc/alpc_bus.h
  DEPENDS
    .nt_ipc
    libc.hdr.types.pid_t
    libc.hdr.types.uid_t
    libc.src.__support.macros.config
)

add_object_library(
  alpc_bus
  SRCS
    ipc/alpc_bus.cpp
    ipc/alpc_bus_subsystem.cpp
  DEPENDS
    .alpc_bus_h
    .alpc_bus_state_h
    .libc_fini_registry
    .nt_ipc
    .nt_security
    .nt_process_api
    .ntdll
    .page_alloc
    .reactor
    .scoped_nt_handle
    .handle_attributes
    .sid_utils
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.span
    libc.src.__support.macros.config
)

# --- Object libraries ---

# ThreadSignalState lives inline inside ThreadLifecycle (lc->sig_state),
# so no separate signal-state slab pool is needed. The previous
# signal_pool object library has been removed.

# New sigqueue pool (SlabPool-backed, no fixed capacity limit).
add_object_library(
  signal_sigqueue_pool
  SRCS
    signal/pending/sigqueue_pool.cpp
  DEPENDS
    .signal_sigqueue_pool_h
    .signal_rt_queue_h
    libc.src.__support.CPP.atomic
    .slab_pool
    .slab_registry
    libc.src.__support.macros.config
)

# Layer 1: Pending storage operations (clear_signal, clear_all).
add_object_library(
  signal_pending_storage
  SRCS
    signal/pending/pending_storage.cpp
  DEPENDS
    .signal_pending_storage_h
    .signal_sigqueue_pool_h
    libc.hdr.signal_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

add_object_library(
  signal_altstack
  SRCS
    signal/signal_altstack.cpp
  DEPENDS
    .signal_internal_h
)

# Layer 2: Transport implementations.
add_object_library(
  signal_transports
  SRCS
    signal/transport/veh_transport.cpp
    signal/transport/console_transport.cpp
    signal/transport/apc_transport.cpp
    signal/transport/alpc_transport.cpp
  DEPENDS
    .signal_veh_transport_h
    .signal_console_transport_h
    .signal_apc_transport_h
    .signal_alpc_transport_h
    .signal_dispatch_engine_h
    .signal_dispatch_fwd_h
    .signal_internal_h
    .signal_pending_storage_h
    .signal_sigqueue_pool_h
    .alpc_bus_h
    .alpc_bus
    .apc
    .crash_handler_ops
    .hw_breakpoint_ops
    .nt_types
    .nt_ipc
    .ntdll
    .reactor
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.pid_t
    libc.hdr.types.siginfo_t
    libc.hdr.types.uid_t
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.thread_lifecycle
    libc.src.__support.threads.windows.thread_registry
    libc.src.__support.macros.config
)

# Layer 3: Dispatch engine + handler table.
add_object_library(
  signal_dispatch_engine
  SRCS
    signal/dispatch/dispatch_engine.cpp
    signal/dispatch/handler_table.cpp
  DEPENDS
    .signal_dispatch_engine_h
    .signal_handler_table_h
    .signal_alt_stack_h
    .signal_restart_state_h
    .signal_process_control_h
    .signal_internal_h
    .signal_pending_storage_h
    .signal_sigqueue_pool_h
    .apc
    libc.hdr.signal_macros
    libc.hdr.types.siginfo_t
    libc.hdr.types.struct_sigaction
    libc.hdr.types.ucontext_t
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.thread_lifecycle
    libc.src.__support.threads.windows.thread_registry
    libc.src.__support.macros.config
)

# Layer 4: Process control (stop/continue, SIGCHLD, default actions).
add_object_library(
  signal_process_control
  SRCS
    signal/control/process_control.cpp
    signal/control/sigchld.cpp
  DEPENDS
    .signal_process_control_h
    .signal_sigchld_h
    .signal_payload
    .signal_dispatch_engine_h
    .signal_restart_state_h
    .signal_internal_h
    .signal_pending_storage_h
    .crash_handler_ops
    .apc
    libc.hdr.signal_macros
    libc.hdr.types.siginfo_t
    libc.src.__support.CPP.atomic
    libc.src.__support.threads.windows.thread_lifecycle
    libc.src.__support.threads.windows.thread_registry
    libc.src.__support.macros.config
)

# Signal subsystem core — lifecycle management + context conversion.
add_object_library(
  signal_subsystem
  SRCS
    signal/signal_state.cpp
    signal/signal_delivery.cpp
  HDRS
    signal/signal.h
  DEPENDS
    .libc_fini_registry
    .signal_internal_h
    .signal_sigqueue_pool
    .signal_pending_storage
    .signal_altstack
    .signal_transports
    .signal_dispatch_engine
    .signal_process_control
    .signal_process_control_h
    .signal_payload
    .signal_dispatch_engine_h
    .signal_sigqueue_pool_h
    .signal_alpc_transport_h
    .signal_veh_transport_h
    .signal_console_transport_h
    .teb_tls
    .tls_cleanup
    .veh_core
    libc.src.__support.threads.windows.robust_list_cleanup
    libc.src.setjmp.longjmp
    .apc
    libc.src.__support.CPP.bit
)

# Signal syscall engine functions — implements signal_state:: kernel
# functions and internal:: engine functions using the four-layer architecture.
add_header_library(
  signal_syscalls_h
  HDRS
    signal/signal_syscalls.h
  DEPENDS
    .signal_h
    libc.hdr.types.pid_t
    libc.hdr.types.siginfo_t
    libc.hdr.types.sigset_t
    libc.hdr.types.struct_timespec
    libc.hdr.types.union_sigval
    libc.src.__support.macros.config
)

add_object_library(
  signal_syscalls
  SRCS
    signal/signal_syscalls.cpp
  DEPENDS
    .condrv
    .process_control_block
    .signal_internal_h
    .signal_syscalls_h
    .signal_subsystem
    .signal_dispatch_engine_h
    .signal_handler_table_h
    .signal_pending_storage_h
    .signal_sigqueue_pool_h
    .signal_process_control_h
    .signal_sigchld_h
    .signal_apc_transport_h
    .signal_alpc_transport_h
    .signal_veh_transport_h
    .signal_console_transport_h
    libc.src.__support.threads.windows.thread_lifecycle
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.pid_t
    libc.hdr.types.siginfo_t
    libc.hdr.types.sigset_t
    libc.hdr.types.stack_t
    libc.hdr.types.struct_sigaction
    libc.hdr.types.struct_timespec
    libc.hdr.types.ucontext_t
    libc.hdr.types.union_sigval
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.process.windows.child_table
)

# ---------------------------------------------------------------------------
# Mmap engine — core mmap logic (placeholder → section → view model).
# Extracted from src/sys/mman/windows/mmap.cpp (Phase 2.2).
# Entry point remains there as a thin wrapper.
# ---------------------------------------------------------------------------

add_object_library(
  mmap_engine
  SRCS
    memory/legacy/mmap_engine.cpp
  HDRS
    memory/legacy/mmap_engine.h
  DEPENDS
    .fd_table
    .nt_pal
    .ntdll
    .mapping_table
    .region_reconcile
    .remap_transaction
    .memory_lock_policy_state
    .page_size
    libc.src.__support.CPP.bit
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
)

# ---------------------------------------------------------------------------
# VM protect engine — munmap + mprotect kernel functions.
# Extracted from src/sys/mman/windows/munmap.cpp and mprotect.cpp (Phase 2.2).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

add_object_library(
  vm_protect
  SRCS
    memory/legacy/vm_protect.cpp
  HDRS
    memory/legacy/vm_protect.h
  DEPENDS
    .nt_pal
    .ntdll
    .mapping_table
    .remap_transaction
    .page_size
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# Program break (brk/sbrk) — demand-growing linear heap segment.
# Sits directly on page_alloc.h, independent of mmap and mapping_table.
# ---------------------------------------------------------------------------

add_object_library(
  brk_state
  SRCS
    memory/legacy/brk_state.cpp
  HDRS
    memory/legacy/brk_state.h
  DEPENDS
    .fork_reinit
    .page_alloc
    .page_size
    .process_control_block
    .rlimit_state
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Pkey operations engine — pkey_alloc, pkey_free, pkey_get, pkey_set.
# Extracted from src/sys/mman/windows/pkey_*.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  pkey_ops
  SRCS
    security/pkey_ops.cpp
  HDRS
    security/pkey_ops.h
  DEPENDS
    .fork_reinit
    libc.src.__support.macros.config
    libc.hdr.errno_macros
)

# ---------------------------------------------------------------------------
# Memory locking engine — mlock, mlock2, munlock (POSIX-layer rebuild,
# memory/posix/mlock.cpp) plus the legacy mlockall / munlockall paths
# that stay quarantined until P9 of the POSIX_ROADMAP rebuilds them on
# the new substrate. The single object library combines both so the
# windows_syscalls aggregate keeps one .mlock_ops dependency edge.
# ---------------------------------------------------------------------------

add_object_library(
  mlock_ops
  SRCS
    memory/posix/mlock.cpp
    memory/legacy/mlock_ops.cpp
  HDRS
    memory/posix/mlock.h
    memory/legacy/mlock_ops.h
  DEPENDS
    .memory_posix_errno
    .memory_posix_validation
    .nt_pal
    .ntdll
    .page_size
    .working_set
    .memory_lock_policy_state
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# madvise engine — MADV_* advisory hint implementation.
# Extracted from src/sys/mman/windows/madvise.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  madvise_ops
  SRCS
    memory/legacy/madvise_ops.cpp
  HDRS
    memory/legacy/madvise_ops.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# posix_madvise engine — POSIX advisory hints (subset of madvise).
# Extracted from src/sys/mman/windows/posix_madvise.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  posix_madvise_ops
  SRCS
    memory/legacy/posix_madvise_ops.cpp
  HDRS
    memory/legacy/posix_madvise_ops.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# msync engine — file-backed memory synchronisation.
# Extracted from src/sys/mman/windows/msync.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  msync_ops
  SRCS
    memory/legacy/msync_ops.cpp
  HDRS
    memory/legacy/msync_ops.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    .mapping_table
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# mincore engine — POSIX-layer page residency probe
# (memory/posix/mincore.cpp on the read-only nt_pal surface).
# ---------------------------------------------------------------------------

add_object_library(
  mincore_ops
  SRCS
    memory/posix/mincore.cpp
  HDRS
    memory/posix/mincore.h
  DEPENDS
    .memory_posix_validation
    .nt_pal
    .ntdll
    .page_size
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
)

# ---------------------------------------------------------------------------
# Shared memory engine — shm_open, shm_unlink, memfd_create.
# Extracted from src/sys/mman/windows/shm_*.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  shm_ops
  SRCS
    memory/legacy/shm_ops.cpp
  HDRS
    memory/legacy/shm_ops.h
  DEPENDS
    .fd_ops
    .ntdll
    .ntdll
    .fd_table
    .temp_path
    libc.src.__support.common
    libc.src.__support.CPP.span
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# SysV shared memory engine — shmget, shmat, shmdt, shmctl.
# ---------------------------------------------------------------------------

add_object_library(
  sysv_shm_ops
  SRCS
    memory/legacy/sysv_shm_ops.cpp
  HDRS
    memory/legacy/sysv_shm_ops.h
  DEPENDS
    .ntdll
    .process_control_block
    .section_region
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.hdr.types.gid_t
    libc.hdr.types.key_t
    libc.hdr.types.size_t
    libc.hdr.types.pid_t
    libc.hdr.types.time_t
    libc.hdr.types.mode_t
    libc.hdr.types.uid_t
    libc.hdr.types.struct_shmid_ds
    libc.include.llvm-libc-macros.sys_ipc_macros
    libc.include.llvm-libc-macros.sys_shm_macros
)

# ---------------------------------------------------------------------------
# SysV semaphore engine — semget, semctl, semop.
# ---------------------------------------------------------------------------

add_object_library(
  sysv_sem_ops
  SRCS
    ipc/sysv_sem_ops.cpp
  HDRS
    ipc/sysv_sem_ops.h
  DEPENDS
    .ntdll
    .process_control_block
    .section_region
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.hdr.errno_macros
    libc.hdr.types.gid_t
    libc.hdr.types.key_t
    libc.hdr.types.mode_t
    libc.hdr.types.pid_t
    libc.hdr.types.size_t
    libc.hdr.types.struct_semid_ds
    libc.hdr.types.struct_sembuf
    libc.hdr.types.time_t
    libc.hdr.types.uid_t
    libc.include.llvm-libc-macros.sys_ipc_macros
    libc.include.llvm-libc-macros.sys_sem_macros
)

# ---------------------------------------------------------------------------
# NUMA policy engine — set_mempolicy, get_mempolicy, mbind.
# Extracted from src/sys/mman/windows/ NUMA files (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  numa_ops
  SRCS
    memory/legacy/numa_ops.cpp
  HDRS
    memory/legacy/numa_ops.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    .mapping_table
    .remap_guard
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# remap_file_pages engine — transactional page remap.
# Extracted from src/sys/mman/windows/remap_file_pages.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  remap_file_pages_ops
  SRCS
    memory/legacy/remap_file_pages_ops.cpp
  HDRS
    memory/legacy/remap_file_pages_ops.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    .mapping_table
    .remap_guard
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# mremap engine — remapping/resizing mapped regions.
# Extracted from src/sys/mman/windows/mremap.cpp (Phase 3.3).
# ---------------------------------------------------------------------------

add_object_library(
  mremap_engine
  SRCS
    memory/legacy/mremap_engine.cpp
  HDRS
    memory/legacy/mremap_engine.h
  DEPENDS
    .nt_pal
    .ntdll
    .ntdll
    .page_size
    .mapping_table
    .remap_transaction
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.hdr.errno_macros
    libc.include.llvm-libc-macros.sys_mman_macros
)

# ---------------------------------------------------------------------------
# Socket engine — internal:: kernel functions for socket operations.
# Extracted from src/sys/socket/windows/ (Phase 2.4).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

# Socket engine header (forward declarations for all internal:: socket functions)
add_header_library(
  socket_engine_h
  HDRS
    ipc/socket_engine.h
  DEPENDS
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.hdr.types.struct_msghdr
    libc.src.__support.macros.config
)

# Socket lifecycle: socket, bind, listen, connect, accept, shutdown, socketpair
add_object_library(
  socket_lifecycle
  SRCS
    ipc/socket_lifecycle.cpp
  DEPENDS
    .afd_core
    .afd_io_completion
    .af_ops
    .af_resolve
    .fd_table
    .fifo_channel
    .ntdll
    .nt_afd
    .sockpair_channel
    .socket_state
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.scope_guard
    libc.src.__support.macros.config
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.include.llvm-libc-macros.sys_socket_macros
)

# Socket data transfer: send, recv, sendmsg, recvmsg
add_object_library(
  socket_data
  SRCS
    ipc/socket_data.cpp
  DEPENDS
    .afd_core
    .fd_table
    .fifo_channel
    .nt_afd
    .read_write
    .signal_h
    .signal_subsystem
    .sockpair_channel
    .socket_state
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.signal_macros
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_msghdr
    libc.hdr.types.struct_sockaddr
    libc.hdr.types.struct_sockaddr_un
    libc.include.llvm-libc-macros.sys_socket_macros
)

# Socket queries: getsockopt, setsockopt, getsockname, getpeername
add_object_library(
  socket_query
  SRCS
    ipc/socket_query.cpp
  DEPENDS
    .afd_core
    .af_ops
    .fd_table
    .fifo_channel
    .nt_afd
    .socket_state
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.hdr.errno_macros
    libc.hdr.types.socklen_t
    libc.hdr.types.struct_sockaddr
    libc.include.llvm-libc-macros.sys_socket_macros
)

# Working set quota expansion and lockability checks for mlock/mlockall
add_header_library(
  working_set
  HDRS
    memory/legacy/working_set.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.config
)

# Explicitly-owned MLOCK_ONFAULT state. VEH filter registered into the
# unified dispatch table at VEH_PRIORITY_MLOCK via a `.libcveh` record;
# Tier B Phase 3's register_all_static_veh_filters() installs it after
# mlock_policy_startup_init() constructs the onfault state.
add_object_library(
  memory_lock_policy_state
  SRCS
    memory/legacy/memory_lock_policy.cpp
  HDRS
    memory/legacy/memory_lock_policy.h
  DEPENDS
    .libc_fini_registry
    .nt_pal
    .ntdll
    .page_size
    .veh_core
    .veh_filter_registry
    .working_set
    libc.src.__support.CPP.new
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.futex_utils
)

add_object_library(
  windows_util
  SRCS
    process/exit.cpp
    io.cpp
  HDRS
    io.h
  DEPENDS
    libc.src.__support.macros.config
    .ntdll
    .read_write
    .syscall
)

# Forward declarations for internal::open/close/lseek/dup2/openat from
# fcntl.cpp. Breaks the include cycle that prevents callers from including
# the implementation headers directly (fd_table -> osutil -> syscall).
add_header_library(
  fd_ops
  HDRS
    io/fd_ops.h
  DEPENDS
    libc.hdr.types.mode_t
    libc.hdr.types.off_t
    libc.src.__support.macros.config
)

add_object_library(
  virtual_fs
  SRCS
    io/virtual_fs.cpp
  HDRS
    io/virtual_fs.h
  DEPENDS
    .fd_table
    .vt_pty
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.macros.config
    libc.src.string.string_utils
)

# POSIX byte-range lock tracking table for fork/close/merge-split semantics.
# Also registers the OP_LOCK_QUERY handler on the ALPC bus so remote peers
# can attest our lock ownership for F_GETLK.
add_object_library(
  fcntl_lock_table
  SRCS
    fcntl_lock_table.cpp
  HDRS
    fcntl_lock_table.h
  DEPENDS
    .alpc_bus_h
    .alpc_bus
    .libc_fini_registry
    .ntdll
    .page_alloc
    .bcryptprimitives
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
)

# Separate from windows_util to avoid a dep cycle:
# windows_util → fd_table → File.file → clock_gettime → windows_util.
add_object_library(
  fcntl
  SRCS
    fcntl.cpp
  DEPENDS
    libc.src.__support.macros.config
    .ntdll
    .fd_table
    .fcntl_lock_table
    .nt_path
    .virtual_fs
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.stdio_macros
    libc.hdr.types.mode_t
    libc.hdr.types.off_t
    libc.hdr.types.struct_flock
)

# Data transfer kernel functions (read/write). Separate from fcntl
# (file control) — matches Linux kernel's fs/read_write.c vs fs/open.c.
# Same pattern as fcntl: fd_table dep, kernel convention (-errno), linked
# into final libc.a, forward-declared in syscall.h.
add_object_library(
  read_write
  SRCS
    io/read_write.cpp
  HDRS
    io/read_write.h
  DEPENDS
    libc.src.__support.macros.config
    .ntdll
    .fd_table
    .fifo_channel
    .io_ring_helpers
    .inotify_ops
    .rlimit_query
    .sockpair_channel
    .console_tty
    .signal_syscalls
    .terminal_ops
    .thread_ring
    .vt_pty
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.signal_macros
    libc.src.__support.File.file
    libc.src.pthread.cancel_internal
)

# Process Control Block — PE section instantiation, Zone 0 seal/unseal,
# PCB Zone 0 seal/unseal, fork reinit support, static_assert validation.
add_object_library(
  process_control_block
  SRCS
    process_control_block.cpp
  HDRS
    process_control_block.h
    pcb_init_access.h
  DEPENDS
    .nt_pal_process_state
    .ntdll
    .page_alloc
    .fork_reinit
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
)

# DLL-mode AppProperties definition and pcb_startup_init() (Phase 0).
# Selectany: overridden by do_start.cpp's strong definition in static EXE mode.
add_object_library(
  app_init
  SRCS
    process/app_init.cpp
  DEPENDS
    .bcryptprimitives
    .nt_capabilities
    .ntdll
    .process_control_block
    .va_substrate
    libc.config.app_h
    libc.src.__support.macros.config
)

# Per-thread IoRing — one ring shared by all fds on a thread.
# FLS-based with cleanup callback. Lazy-created on first I/O.
add_object_library(
  thread_ring
  SRCS
    io/thread_ring.cpp
  HDRS
    io/thread_ring.h
  DEPENDS
    .nt_pal
    .ntdll
    .nt_ioring_ops
    .slab_pool
    .teb_tls
    .tls_cleanup
    libc.src.__support.macros.config
)

# Itanium __cxa_guard for c.dll — loads before libc++abi.
add_object_library(
  cxa_guard
  SRCS
    process/cxa_guard.cpp
  DEPENDS
    .ntdll
    libc.src.__support.macros.config
)

add_object_library(
  ofd_pool
  SRCS
    fd/ofd_pool.cpp
  HDRS
    fd/ofd_pool.h
  DEPENDS
    .libc_fini_registry
    .ntdll
    .slab_pool
    .slab_registry
    .fork_reinit
    libc.src.__support.macros.config
)

add_object_library(
  file_pool
  SRCS
    fd/file_pool.cpp
  HDRS
    fd/file_pool.h
  DEPENDS
    .libc_fini_registry
    .ntdll
    .slab_pool
    .slab_registry
    .fork_reinit
    libc.src.__support.macros.config
)

add_header_library(
  device_path
  HDRS
    device_path.h
  DEPENDS
    .ntdll
    .fd_table
    libc.src.__support.macros.config
)

add_object_library(
  fd_table
  SRCS
    fd/fd_table.cpp
    fd/file_ops_table.cpp
  HDRS
    fd/fd_table.h
    fd/file_ops_table.h
    fd/file_ops_section.h
  DEPENDS
    .alloc_guarded_region
    .alloc_init_latch
    .alloc_occupancy_bitmap
    .fcntl_lock_table
    .fifo_channel
    .file_pool
    .libc_fini_registry
    .ntdll
    .ofd_pool
    .process_control_block
    .rlimit_query
    .section_registry
    .sockpair_channel
    .socket_state
    .thread_ring
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.File.windows.windows_file_ops_table
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
    libc.src.stdio.stdin
    libc.src.stdio.stdout
    libc.src.stdio.stderr
)

#===============================================================================
# Import Library Generation from .def files
#===============================================================================
# Generate import libraries at build time using llvm-dlltool.
# This avoids depending on Windows SDK .lib files.

set(WINDOWS_IMPORT_LIBS_DIR "${CMAKE_CURRENT_BINARY_DIR}/import_libs")
file(MAKE_DIRECTORY ${WINDOWS_IMPORT_LIBS_DIR})
file(MAKE_DIRECTORY ${LIBC_LIBRARY_DIR})

# Prefer llvm-dlltool when available, but allow standalone Windows builds
# driven from a VS dev shell to use lib.exe as a fallback.
find_program(WINDOWS_IMPORT_LIB_TOOL NAMES llvm-dlltool llvm-dlltool.exe lib
                                            lib.exe)
if(NOT WINDOWS_IMPORT_LIB_TOOL)
  message(FATAL_ERROR
          "Could not find llvm-dlltool or lib.exe for Windows import lib generation.")
endif()
get_filename_component(WINDOWS_IMPORT_LIB_TOOL_NAME "${WINDOWS_IMPORT_LIB_TOOL}"
                       NAME_WE)

# Machine type for import library generation.
if(LIBC_TARGET_ARCHITECTURE STREQUAL "aarch64")
  set(DLLTOOL_MACHINE "arm64")
  set(MSVC_IMPORT_MACHINE "ARM64")
else()
  set(DLLTOOL_MACHINE "i386:x86-64")
  set(MSVC_IMPORT_MACHINE "X64")
endif()

# Function to add an import library target
function(add_windows_import_lib name)
  set(def_file "${CMAKE_CURRENT_SOURCE_DIR}/imports/${name}.def")
  set(lib_file "${WINDOWS_IMPORT_LIBS_DIR}/${name}.lib")

  if(WINDOWS_IMPORT_LIB_TOOL_NAME STREQUAL "llvm-dlltool")
    set(import_lib_command "${WINDOWS_IMPORT_LIB_TOOL}" -m ${DLLTOOL_MACHINE}
                           -d ${def_file} -l ${lib_file})
  else()
    set(import_lib_command "${WINDOWS_IMPORT_LIB_TOOL}" /nologo
                           /machine:${MSVC_IMPORT_MACHINE} /def:${def_file}
                           /out:${lib_file})
  endif()

  add_custom_command(
    OUTPUT ${lib_file}
    COMMAND ${import_lib_command}
    DEPENDS ${def_file}
    COMMENT "Generating ${name}.lib from ${name}.def"
    VERBATIM
  )

  add_custom_target(${name}_importlib DEPENDS ${lib_file})

  # Create an imported library target
  add_library(windows_${name} STATIC IMPORTED GLOBAL)
  set_target_properties(windows_${name} PROPERTIES
    IMPORTED_LOCATION ${lib_file}
  )
  add_dependencies(windows_${name} ${name}_importlib)
endfunction()

function(stage_windows_import_lib name)
  set(lib_file "${WINDOWS_IMPORT_LIBS_DIR}/${name}.lib")
  set(staged_dir "${LIBC_LIBRARY_DIR}")
  set(staged_file "${staged_dir}/${name}.lib")
  if((WIN32_ITANIUM OR WIN32_NTPOSIX) AND LLVM_LIBRARY_OUTPUT_INTDIR)
    set(staged_dir "${LLVM_LIBRARY_OUTPUT_INTDIR}")
    set(staged_file "${staged_dir}/${name}.lib")
  endif()

  add_custom_command(
    OUTPUT ${staged_file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${staged_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${lib_file} ${staged_file}
    DEPENDS ${name}_importlib
    COMMENT "Staging ${name}.lib for ambient Windows Itanium linking"
    VERBATIM
  )
  add_custom_target(${name}_ambient_importlib DEPENDS ${staged_file})
endfunction()

# NLS locale blob parser — fork-safe locale data from ntdll.
add_object_library(
  nls_locale
  SRCS
    nls_locale.cpp
  HDRS
    nls_locale.h
  DEPENDS
    .lazy_init
    .ntdll
    libc.src.__support.CPP.span
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
    libc.src.__support.macros.config
    libc.include.llvm-libc-types.struct_lconv
)

# ---------------------------------------------------------------------------
# Syscall wrappers — windows_syscalls:: namespace, mirrors linux_syscalls::
#
# Header-only libraries returning ErrorOr<T>. Thin wrappers that call
# internal:: engine functions (for complex ops) or inline trivial operations
# (for TEB reads, stubs, etc.).
# ---------------------------------------------------------------------------

add_header_library(
  syscall_wrapper_read
  HDRS
    syscall_wrappers/read.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.ssize_t
)

add_header_library(
  syscall_wrapper_write
  HDRS
    syscall_wrappers/write.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.ssize_t
)

add_header_library(
  syscall_wrapper_close
  HDRS
    syscall_wrappers/close.h
  DEPENDS
    .fcntl
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_lseek
  HDRS
    syscall_wrappers/lseek.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
)

add_header_library(
  syscall_wrapper_dup2
  HDRS
    syscall_wrappers/dup2.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_dup
  HDRS
    syscall_wrappers/dup.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_execveat
  HDRS
    syscall_wrappers/execveat.h
  DEPENDS
    .exec_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_open
  HDRS
    syscall_wrappers/open.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.mode_t
)

add_header_library(
  syscall_wrapper_fcntl
  HDRS
    syscall_wrappers/fcntl.h
  DEPENDS
    .fcntl
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_getpid
  HDRS
    syscall_wrappers/getpid.h
  DEPENDS
    .ntdll
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.pid_t
)

add_header_library(
  syscall_wrapper_getsid
  HDRS
    syscall_wrappers/getsid.h
  DEPENDS
    .console
    .ntdll
    libc.hdr.errno_macros
    libc.hdr.types.pid_t
    libc.src.__support.common
    libc.src.__support.CPP.atomic
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_setpgid
  HDRS
    syscall_wrappers/setpgid.h
  DEPENDS
    .ntdll
    .syscall_wrapper_getsid
    libc.hdr.errno_macros
    libc.hdr.types.pid_t
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.src.__support.process.windows.child_table
)

add_header_library(
  syscall_wrapper_gettid
  HDRS
    syscall_wrappers/gettid.h
  DEPENDS
    .ntdll
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.pid_t
)

add_header_library(
  syscall_wrapper_getuid
  HDRS
    syscall_wrappers/getuid.h
  DEPENDS
    .process_control_block
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
)

add_header_library(
  syscall_wrapper_geteuid
  HDRS
    syscall_wrappers/geteuid.h
  DEPENDS
    .process_control_block
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
)

add_header_library(
  syscall_wrapper_getgid
  HDRS
    syscall_wrappers/getgid.h
  DEPENDS
    .process_control_block
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
)

add_header_library(
  syscall_wrapper_getegid
  HDRS
    syscall_wrappers/getegid.h
  DEPENDS
    .process_control_block
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
)

add_header_library(
  syscall_wrapper_getppid
  HDRS
    syscall_wrappers/getppid.h
  DEPENDS
    .ntdll
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.pid_t
    libc.hdr.errno_macros
)

add_header_library(
  syscall_wrapper_getgroups
  HDRS
    syscall_wrappers/getgroups.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
    libc.hdr.errno_macros
)

add_header_library(
  syscall_wrapper_setreuid
  HDRS
    syscall_wrappers/setreuid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
    libc.hdr.errno_macros
)

add_header_library(
  syscall_wrapper_setregid
  HDRS
    syscall_wrappers/setregid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
    libc.hdr.errno_macros
)

# File I/O internal operations (dup3, pread, pwrite, fsync, fdatasync,
# ftruncate, truncate, isatty, pipe2).
add_object_library(
  file_ops
  SRCS
    io/file_ops.cpp
  HDRS
    io/file_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.off_t
    .fd_table
    .io_ring_helpers
    .ntdll
    .ntdll
    .rlimit_query
)

add_header_library(
  syscall_wrapper_dup3
  HDRS
    syscall_wrappers/dup3.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_copy_file_range
  HDRS
    syscall_wrappers/copy_file_range.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
    libc.hdr.types.size_t
    libc.hdr.types.ssize_t
)

add_header_library(
  syscall_wrapper_pread
  HDRS
    syscall_wrappers/pread.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
    libc.hdr.types.ssize_t
)

add_header_library(
  syscall_wrapper_pwrite
  HDRS
    syscall_wrappers/pwrite.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
    libc.hdr.types.ssize_t
)

add_header_library(
  syscall_wrapper_fsync
  HDRS
    syscall_wrappers/fsync.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_fdatasync
  HDRS
    syscall_wrappers/fdatasync.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_ftruncate
  HDRS
    syscall_wrappers/ftruncate.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
)

add_header_library(
  syscall_wrapper_truncate
  HDRS
    syscall_wrappers/truncate.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.off_t
)

add_header_library(
  syscall_wrapper_pipe2
  HDRS
    syscall_wrappers/pipe2.h
  DEPENDS
    .file_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# Path internal operations (faccessat, unlinkat, linkat, symlinkat,
# readlinkat, chdir, fchdir, getcwd).
add_object_library(
  path_ops
  SRCS
    io/path_ops.cpp
  HDRS
    io/path_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.ssize_t
    .fd_table
    .ntdll
    .nt_path
    .ntdll
    .device_path
    .syscall
)

add_header_library(
  syscall_wrapper_faccessat
  HDRS
    syscall_wrappers/faccessat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_unlinkat
  HDRS
    syscall_wrappers/unlinkat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_linkat
  HDRS
    syscall_wrappers/linkat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_symlinkat
  HDRS
    syscall_wrappers/symlinkat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_readlinkat
  HDRS
    syscall_wrappers/readlinkat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.ssize_t
    .path_ops
)

add_header_library(
  syscall_wrapper_chdir
  HDRS
    syscall_wrappers/chdir.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_fchdir
  HDRS
    syscall_wrappers/fchdir.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

add_header_library(
  syscall_wrapper_getcwd
  HDRS
    syscall_wrappers/getcwd.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .path_ops
)

# POSIX group database operations (getgrgid, getgrnam).
add_object_library(
  group_ops
  SRCS
    process/group_ops.cpp
  HDRS
    process/group_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.gid_t
    libc.hdr.types.size_t
    libc.hdr.types.struct_group
    .ntdll
    .process_control_block
)

# POSIX passwd database operations (getpwuid, getpwnam).
add_object_library(
  passwd_ops
  SRCS
    process/passwd_ops.cpp
  HDRS
    process/passwd_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    libc.hdr.types.size_t
    libc.hdr.types.struct_passwd
    .ntdll
    .process_control_block
)

# Process identity internal operations (setuid, seteuid, setgid, setegid).
add_object_library(
  identity_ops
  SRCS
    process/identity_ops.cpp
  HDRS
    process/identity_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    .ntdll
    .process_control_block
    .process_identity
    .signal_subsystem
)

add_header_library(
  syscall_wrapper_setuid
  HDRS
    syscall_wrappers/setuid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
)

add_header_library(
  syscall_wrapper_seteuid
  HDRS
    syscall_wrappers/seteuid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
)

add_header_library(
  syscall_wrapper_setgid
  HDRS
    syscall_wrappers/setgid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
)

add_header_library(
  syscall_wrapper_setegid
  HDRS
    syscall_wrappers/setegid.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.gid_t
)

# File ownership internal operations (chown, fchown, fchownat).
add_object_library(
  ownership_ops
  SRCS
    security/ownership_ops.cpp
  HDRS
    security/ownership_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    .fd_table
    .nt_path
    .security
)

add_header_library(
  syscall_wrapper_chown
  HDRS
    syscall_wrappers/chown.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    .ownership_ops
)

add_header_library(
  syscall_wrapper_fchown
  HDRS
    syscall_wrappers/fchown.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.uid_t
    libc.hdr.types.gid_t
    .ownership_ops
)

add_header_library(
  syscall_wrapper_fchownat
  HDRS
    syscall_wrappers/fchownat.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.fcntl_macros
    libc.hdr.types.uid_t
    .ownership_ops
    libc.hdr.types.gid_t
)

# Misc internal operations (getentropy, gethostname, sysconf, pathconf, fpathconf).
# Out-of-line engine code — forward-declared in io/misc_ops.h, resolved at link.
add_object_library(
  misc_ops
  SRCS
    io/misc_ops.cpp
  HDRS
    io/misc_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.limits_macros
    libc.hdr.unistd_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.error_or
    libc.src.__support.macros.config
    .bcryptprimitives
    .fd_table
    .nt_path
    .ntdll
    .page_size
)

# ---------------------------------------------------------------------------
# Wait engine — POSIX wait family implementation (waitid, waitpid, wait4).
# Extracted from src/sys/wait/windows/ (Phase 3.8).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

add_object_library(
  wait_ops
  SRCS
    process/wait_ops.cpp
  HDRS
    process/wait_ops.h
    process/wait_utils.h
  DEPENDS
    .ntdll
    .ntdll
    libc.hdr.errno_macros
    libc.hdr.types.id_t
    libc.hdr.types.idtype_t
    libc.hdr.types.pid_t
    libc.hdr.types.struct_rusage
    libc.include.signal
    libc.include.sys_wait
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.process.windows.child_table
    libc.src.__support.OSUtil.windows.signal_subsystem
)

add_header_library(
  syscall_wrapper_waitpid
  HDRS
    syscall_wrappers/waitpid.h
  DEPENDS
    .wait_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.pid_t
)

add_header_library(
  syscall_wrapper_wait4
  HDRS
    syscall_wrappers/wait4.h
  DEPENDS
    .wait_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.pid_t
    libc.hdr.types.struct_rusage
)

add_header_library(
  syscall_wrapper_waitid
  HDRS
    syscall_wrappers/waitid.h
  DEPENDS
    .wait_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.id_t
    libc.hdr.types.idtype_t
    libc.include.signal
)

# ---------------------------------------------------------------------------
# Poll engine — AFD-based poll implementation.
# Extracted from src/poll/windows/poll.cpp (Phase 3.10).
# Entry point remains there as a thin wrapper.
# ---------------------------------------------------------------------------

add_object_library(
  poll_ops
  SRCS
    ipc/poll_ops.cpp
  HDRS
    ipc/poll_ops.h
  DEPENDS
    .afd_core
    .fd_table
    .fifo_channel
    .nt_afd
    .ntdll
    .page_alloc
    .sockpair_channel
    .socket_state
    libc.hdr.types.nfds_t
    libc.hdr.types.sigset_t
    libc.hdr.types.struct_pollfd
    libc.hdr.types.struct_timespec
    libc.include.llvm-libc-macros.poll-macros
    libc.src.__support.common
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.scope_guard
    libc.src.__support.macros.config
    libc.src.pthread.cancel_internal
    libc.src.__support.OSUtil.windows.signal_subsystem
)

add_header_library(
  syscall_wrapper_poll
  HDRS
    syscall_wrappers/poll.h
  DEPENDS
    .poll_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.nfds_t
)

# ---------------------------------------------------------------------------
# Epoll engine — IOCP-based epoll implementation.
# Extracted from src/sys/epoll/windows/ (Phase 3.7).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

add_object_library(
  epoll_ops
  SRCS
    ipc/epoll_ops.cpp
  HDRS
    ipc/epoll_ops.h
  DEPENDS
    .completion_router
    .fd_table
    .fifo_channel
    .nt_afd
    .nt_file
    .ntdll
    .socket_epoll
    .sockpair_channel
    libc.hdr.types.struct_epoll_event
    libc.include.sys_epoll
    libc.src.__support.common
    libc.src.__support.macros.config
)

add_object_library(
  inotify_ops
  SRCS
    ipc/inotify_ops.cpp
  HDRS
    ipc/inotify_ops.h
    ipc/inotify_instance.h
  DEPENDS
    .fd_table
    .nt_file
    .ntdll
    .page_alloc
    .reactor
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.CPP.scope_guard
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
)

add_header_library(
  syscall_wrapper_epoll_create1
  HDRS
    syscall_wrappers/epoll_create1.h
  DEPENDS
    .epoll_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_epoll_ctl
  HDRS
    syscall_wrappers/epoll_ctl.h
  DEPENDS
    .epoll_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.struct_epoll_event
)

add_header_library(
  syscall_wrapper_epoll_wait
  HDRS
    syscall_wrappers/epoll_wait.h
  DEPENDS
    .epoll_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.hdr.types.struct_epoll_event
)

add_header_library(
  syscall_wrapper_inotify_init1
  HDRS
    syscall_wrappers/inotify_init1.h
  DEPENDS
    .inotify_ops
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_inotify_add_watch
  HDRS
    syscall_wrappers/inotify_add_watch.h
  DEPENDS
    .inotify_ops
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_inotify_rm_watch
  HDRS
    syscall_wrappers/inotify_rm_watch.h
  DEPENDS
    .inotify_ops
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Rename/remove engine — file removal and renaming (remove, rename, renameat).
# Extracted from src/stdio/windows/ (Phase 3.13).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

add_object_library(
  rename_ops
  SRCS
    io/rename_ops.cpp
  HDRS
    io/rename_ops.h
  DEPENDS
    .fd_table
    .ntdll
    .nt_path
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.common
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_renameat
  HDRS
    syscall_wrappers/renameat.h
  DEPENDS
    .rename_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# stdio file lifecycle engine — fclose/fdopen/freopen/tmpfile.
# Extracted from src/stdio/windows/ (Phase 3.13).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

add_object_library(
  stdio_file_ops
  SRCS
    io/stdio_file_ops.cpp
  HDRS
    io/stdio_file_ops.h
  DEPENDS
    .fd_ops
    .fd_table
    .env_ops
    .file_ops
    .io_ring_helpers
    .ntdll
    .nt_path
    .security
    .spawn_ops
    .temp_path
    .wait_ops
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.stdio_macros
    libc.hdr.unistd_macros
    libc.hdr.types.FILE
    libc.hdr.types.pid_t
    libc.include.spawn
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.new
    libc.src.__support.File.file
    libc.src.__support.File.platform_file
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.spawn.file_actions
    libc.src.unistd.getentropy
)

# ---------------------------------------------------------------------------
# Stat engine — file metadata queries (stat/lstat/fstat/fstatat).
# Extracted from src/sys/stat/windows/ (Phase 3.4).
# Entry points remain there as thin wrappers.
# ---------------------------------------------------------------------------

# nt_to_stat — stat struct conversion helpers (moved from src/sys/stat/windows/).
add_header_library(
  nt_to_stat
  HDRS
    io/nt_to_stat.h
  DEPENDS
    libc.include.sys_stat
    libc.src.__support.OSUtil.windows.fifo
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.security
    libc.src.__support.common
)

# stat/lstat/fstat/fstatat engine — file metadata queries.
add_object_library(
  stat_ops
  SRCS
    io/stat_ops.cpp
  HDRS
    io/stat_ops.h
  DEPENDS
    .nt_to_stat
    .fd_table
    .nt_path
    .ntdll
    libc.include.sys_stat
    libc.src.__support.common
    libc.src.__support.macros.config
)

# chmod/fchmod/fchmodat engine — permission modification.
add_object_library(
  chmod_ops
  SRCS
    security/chmod_ops.cpp
  HDRS
    security/chmod_ops.h
  DEPENDS
    .fd_table
    .ntdll
    .nt_path
    .security
    libc.include.sys_stat
    libc.src.__support.common
    libc.src.__support.macros.config
)

# mkdirat/mkfifo engine — directory and FIFO creation.
add_object_library(
  mkdir_ops
  SRCS
    io/mkdir_ops.cpp
  HDRS
    io/mkdir_ops.h
  DEPENDS
    .fd_table
    .fifo
    .ntdll
    .nt_path
    .security
    libc.hdr.types.mode_t
    libc.include.sys_stat
    libc.src.__support.common
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_stat
  HDRS
    syscall_wrappers/stat.h
  DEPENDS
    .stat_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_lstat
  HDRS
    syscall_wrappers/lstat.h
  DEPENDS
    .stat_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_fstat
  HDRS
    syscall_wrappers/fstat.h
  DEPENDS
    .stat_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_fstatat
  HDRS
    syscall_wrappers/fstatat.h
  DEPENDS
    .stat_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_chmod
  HDRS
    syscall_wrappers/chmod.h
  DEPENDS
    .chmod_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_fchmod
  HDRS
    syscall_wrappers/fchmod.h
  DEPENDS
    .chmod_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_fchmodat
  HDRS
    syscall_wrappers/fchmodat.h
  DEPENDS
    .chmod_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_utimensat
  HDRS
    syscall_wrappers/utimensat.h
  DEPENDS
    .utimes_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_mkdirat
  HDRS
    syscall_wrappers/mkdirat.h
  DEPENDS
    .mkdir_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_mkfifo
  HDRS
    syscall_wrappers/mkfifo.h
  DEPENDS
    .mkdir_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_umask
  HDRS
    syscall_wrappers/umask.h
  DEPENDS
    .security
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# posix_fallocate engine — space preallocation via NtSetInformationFile.
# Extracted from src/fcntl/windows/posix_fallocate.cpp (Phase 3.2).
# ---------------------------------------------------------------------------

add_object_library(
  fallocate_ops
  SRCS
    io/fallocate_ops.cpp
  HDRS
    io/fallocate_ops.h
  DEPENDS
    .fd_table
    .ntdll
    .syscall
    libc.hdr.errno_macros
    libc.hdr.types.off_t
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# mkstemp engine — secure temporary file creation with CSPRNG template fill.
# Extracted from src/stdlib/windows/mkstemp.cpp (Phase 3.14).
# ---------------------------------------------------------------------------

add_object_library(
  mkstemp_ops
  SRCS
    io/mkstemp_ops.cpp
  HDRS
    io/mkstemp_ops.h
  DEPENDS
    .fd_table
    .io_ring_helpers
    .nt_path
    .ntdll
    .security
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.src.__support.macros.config
    libc.src.unistd.getentropy
)

add_object_library(
  mkdtemp_ops
  SRCS
    io/mkdtemp_ops.cpp
  HDRS
    io/mkdtemp_ops.h
  DEPENDS
    .nt_path
    .ntdll
    .security
    libc.hdr.errno_macros
    libc.src.__support.macros.config
    libc.src.string.string_utils
    libc.src.unistd.getentropy
)

add_object_library(
  mktemp_ops
  SRCS
    io/mktemp_ops.cpp
  HDRS
    io/mktemp_ops.h
  DEPENDS
    .nt_path
    .ntdll
    libc.hdr.errno_macros
    libc.src.__support.macros.config
    libc.src.string.string_utils
    libc.src.unistd.getentropy
)

# ---------------------------------------------------------------------------
# Environment variable operations — getenv/setenv/unsetenv/putenv engine.
# Operates against g_pcb.environment under its process-wide lock.
# ---------------------------------------------------------------------------

add_object_library(
  env_ops
  SRCS
    io/env_ops.cpp
  HDRS
    io/env_ops.h
  DEPENDS
    .process_control_block
    .fork_reinit
    libc.hdr.errno_macros
    libc.src.__support.CPP.string_view
    libc.src.__support.libc_errno
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
    libc.src.stdlib.free
    libc.src.stdlib.malloc
    libc.src.unistd.environ
)

# ---------------------------------------------------------------------------
# realpath engine — canonical path resolution via NtQueryObject.
# Extracted from src/stdlib/windows/realpath.cpp (Phase 3.14).
# ---------------------------------------------------------------------------

add_object_library(
  realpath_ops
  SRCS
    io/realpath_ops.cpp
  HDRS
    io/realpath_ops.h
  DEPENDS
    .device_path
    .nt_path
    .ntdll
    libc.hdr.errno_macros
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# libgen engine -- lexical basename/dirname parsing with Windows root forms.
# ---------------------------------------------------------------------------

add_object_library(
  libgen_ops
  SRCS
    io/libgen_ops.cpp
  HDRS
    io/libgen_ops.h
  DEPENDS
    libc.src.__support.CPP.string_view
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Scatter/gather I/O engine — readv/writev via IO ring.
# Extracted from src/sys/uio/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  scatter_io_ops
  SRCS
    io/scatter_io_ops.cpp
  HDRS
    io/scatter_io_ops.h
  DEPENDS
    .fd_table
    .io_ring_helpers
    .rlimit_query
    .signal_h
    .thread_ring
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.signal_macros
    libc.hdr.types.struct_iovec
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# statvfs engine — filesystem statistics via NtQueryVolumeInformation.
# Extracted from src/sys/statvfs/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  statvfs_ops
  SRCS
    io/statvfs_ops.cpp
  HDRS
    io/statvfs_ops.h
  DEPENDS
    .fd_table
    .nt_path
    .ntdll
    .syscall
    libc.hdr.errno_macros
    libc.include.llvm-libc-types.struct_statvfs
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# utimes engine — file timestamp modification via NtSetInformationFile.
# Extracted from src/sys/time/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  utimes_ops
  SRCS
    io/utimes_ops.cpp
  HDRS
    io/utimes_ops.h
  DEPENDS
    .fd_table
    .ntdll
    .nt_path
    libc.hdr.errno_macros
    libc.hdr.types.struct_timeval
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# select engine — POSIX select via AFD polling.
# Extracted from src/sys/select/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  select_ops
  SRCS
    ipc/select_ops.cpp
  HDRS
    ipc/select_ops.h
  DEPENDS
    .fd_table
    .poll_ops
    libc.hdr.errno_macros
    libc.hdr.types.sigset_t
    libc.hdr.types.struct_pollfd
    libc.hdr.types.struct_timespec
    libc.include.llvm-libc-macros.sys_select_macros
    libc.include.llvm-libc-macros.poll-macros
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# sendfile engine — zero-copy file transfer via AFD transmit.
# Extracted from src/sys/sendfile/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  sendfile_ops
  SRCS
    ipc/sendfile_ops.cpp
  HDRS
    ipc/sendfile_ops.h
  DEPENDS
    .afd_core
    .fd_table
    .fifo_channel
    .nt_afd
    .page_alloc
    .sockpair_channel
    .socket_state
    libc.hdr.errno_macros
    libc.src.__support.CPP.scope_guard
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Scheduler helpers — priority mapping and thread lookup (header-only).
# Shared by sched_ops, affinity_ops, scheduler_ops.
# ---------------------------------------------------------------------------

add_header_library(
  sched_helpers
  HDRS
    sched/sched_helpers.h
  DEPENDS
    .ntdll
    .signal_internal_h
    libc.hdr.sched_macros
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.thread_registry
)

# ---------------------------------------------------------------------------
# Scheduler basic ops — sched_yield, getcpu, sched_getcpu,
# sched_get_priority_max/min, sched_rr_get_interval.
# Extracted from src/sched/windows/ (Phase 3.11).
# ---------------------------------------------------------------------------

add_object_library(
  sched_ops
  SRCS
    sched/sched_ops.cpp
  HDRS
    sched/sched_ops.h
  DEPENDS
    .ntdll
    .sched_helpers
    libc.hdr.errno_macros
    libc.hdr.sched_macros
    libc.src.__support.common
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_getcpu
  HDRS
    syscall_wrappers/getcpu.h
  DEPENDS
    .sched_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# CPU affinity ops — sched_getaffinity, sched_setaffinity.
# Extracted from src/sched/windows/ (Phase 3.11).
# ---------------------------------------------------------------------------

add_object_library(
  affinity_ops
  SRCS
    sched/affinity_ops.cpp
  HDRS
    sched/affinity_ops.h
  DEPENDS
    .ntdll
    .sched_helpers
    libc.hdr.errno_macros
    libc.hdr.types.cpu_set_t
    libc.hdr.types.pid_t
    libc.hdr.types.size_t
    libc.src.__support.common
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Scheduler policy ops — sched_getscheduler, sched_setscheduler,
# sched_getparam, sched_setparam.
# Extracted from src/sched/windows/ (Phase 3.11).
# ---------------------------------------------------------------------------

add_object_library(
  scheduler_ops
  SRCS
    sched/scheduler_ops.cpp
  HDRS
    sched/scheduler_ops.h
  DEPENDS
    .ntdll
    .sched_helpers
    libc.hdr.errno_macros
    libc.hdr.sched_macros
    libc.hdr.types.pid_t
    libc.hdr.types.struct_sched_param
    libc.src.__support.common
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Resource limit state — inline helpers operating on g_pcb.rlimit.
# Moved from src/sys/resource/windows/ (Phase 3.17).
# ---------------------------------------------------------------------------

add_header_library(
  rlimit_state
  HDRS
    resource/rlimit_state.h
  DEPENDS
    .ntdll
    .process_control_block
    libc.hdr.types.struct_rlimit
    libc.include.llvm-libc-macros.sys_resource_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.futex_utils
)

add_header_library(
  rlimit_query
  HDRS
    resource/rlimit_query.h
  DEPENDS
    .rlimit_state
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# CPU limit timer — RLIMIT_CPU soft limit enforcement via job IOCP.
# ---------------------------------------------------------------------------

add_object_library(
  cpu_limit_timer
  SRCS
    resource/cpu_limit_timer.cpp
  HDRS
    resource/cpu_limit_timer.h
  DEPENDS
    .fork_reinit
    .ntdll
    .process_control_block
    .reactor
    .rlimit_state
    .signal_subsystem
    libc.hdr.signal_macros
    libc.include.llvm-libc-macros.sys_resource_macros
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Resource ops — getrlimit, setrlimit, prlimit, getrusage.
# Extracted from src/sys/resource/windows/ (Phase 3.17).
# ---------------------------------------------------------------------------

add_object_library(
  resource_ops
  SRCS
    resource/resource_ops.cpp
  HDRS
    resource/resource_ops.h
  DEPENDS
    .cpu_limit_timer
    .fork_reinit
    .ntdll
    .rlimit_state
    .working_set
    libc.hdr.errno_macros
    libc.hdr.types.struct_rlimit
    libc.hdr.types.struct_rusage
    libc.include.llvm-libc-macros.sys_resource_macros
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_getrlimit
  HDRS
    syscall_wrappers/getrlimit.h
  DEPENDS
    .resource_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_setrlimit
  HDRS
    syscall_wrappers/setrlimit.h
  DEPENDS
    .resource_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_getrusage
  HDRS
    syscall_wrappers/getrusage.h
  DEPENDS
    .resource_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_prlimit
  HDRS
    syscall_wrappers/prlimit.h
  DEPENDS
    .resource_ops
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Priority ops — getpriority, setpriority.
# System-wide NtGetNextProcess enumeration with PRIO_PROCESS/PGRP/USER.
# ---------------------------------------------------------------------------

add_object_library(
  priority_ops
  SRCS
    resource/priority_ops.cpp
  HDRS
    resource/priority_ops.h
  DEPENDS
    .ntdll
    .process_control_block
    libc.hdr.errno_macros
    libc.hdr.types.id_t
    libc.hdr.types.uid_t
    libc.include.llvm-libc-macros.sys_resource_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_getpriority
  HDRS
    syscall_wrappers/getpriority.h
  DEPENDS
    .priority_ops
    libc.hdr.types.id_t
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

add_header_library(
  syscall_wrapper_setpriority
  HDRS
    syscall_wrappers/setpriority.h
  DEPENDS
    .priority_ops
    libc.hdr.types.id_t
    libc.src.__support.common
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Clock ops — clock_getres, clock_getcpuclockid.
# Extracted from src/time/windows/ (Phase 3.9).
# ---------------------------------------------------------------------------

add_object_library(
  clock_ops
  SRCS
    time/clock_ops.cpp
  HDRS
    time/clock_ops.h
  DEPENDS
    .ntdll
    .ntdll
    libc.hdr.errno_macros
    libc.hdr.time_macros
    libc.src.__support.CPP.limits
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
    libc.src.__support.time.units
)

# ---------------------------------------------------------------------------
# Interval timer ops — setitimer, getitimer (NT timer + reactor watch).
# Extracted from src/sys/time/windows/ (Phase 3.18).
# ---------------------------------------------------------------------------

add_object_library(
  itimer_ops
  SRCS
    time/itimer_ops.cpp
  HDRS
    time/itimer_ops.h
  DEPENDS
    .fork_reinit
    .ntdll
    .reactor
    .signal_h
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.struct_itimerval
    libc.include.llvm-libc-macros.sys_time_macros
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
)

# ---------------------------------------------------------------------------
# POSIX timer ops — timer_create, timer_settime, timer_gettime,
# timer_getoverrun, timer_delete. Timer manager + reactor backed.
# Extracted from src/time/windows/ (Phase 3.9).
# ---------------------------------------------------------------------------

add_object_library(
  timer_ops
  SRCS
    time/timer_ops.cpp
  HDRS
    time/timer_ops.h
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.time_macros
    libc.hdr.types.struct_itimerspec
    libc.hdr.types.struct_sigevent
    libc.src.__support.CPP.atomic
    libc.src.__support.common
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.timer_manager
)

# ---------------------------------------------------------------------------
# dlfcn engine — dlopen, dlsym, dlclose, dladdr, dlinfo.
# Extracted from src/dlfcn/windows/ (Phase 3.15).
# ---------------------------------------------------------------------------

add_object_library(
  dlfcn_ops
  SRCS
    dlfcn/dlfcn_ops.cpp
  HDRS
    dlfcn/dlfcn_ops.h
  DEPENDS
    .ntdll
    .region_reconcile
    libc.hdr.dlfcn_macros
    libc.hdr.errno_macros
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# r_debug — GDB/LLDB solib rendezvous (_r_debug, _r_debug_pe, link_map,
# link_map_pe). Cross-process debuggers discover loaded modules from these
# exported structs. Lives alongside dlfcn because it shares the PEB->Ldr
# walk; kept in its own object library so veh_core can depend on it without
# pulling the LdrLoadDll path of dlfcn_ops.
# ---------------------------------------------------------------------------

add_object_library(
  r_debug
  SRCS
    dlfcn/r_debug.cpp
  HDRS
    dlfcn/r_debug.h
  DEPENDS
    .ntdll
    .page_alloc
    libc.hdr.stdint_proxy
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# stack walker — custom async-signal-safe x64 PE stack walker.
# Reads .pdata/.xdata directly from mapped PE images.
# ---------------------------------------------------------------------------

add_object_library(
  stack_walker_ops
  SRCS
    debug/stack_walker.cpp
  HDRS
    debug/stack_walker.h
    nt/nt_unwind_types.h
  DEPENDS
    .ntdll
    .veh_core
    libc.src.__support.macros.config
    libc.src.setjmp.setjmp
)

# ---------------------------------------------------------------------------
# bt_format — async-signal-safe backtrace formatting (PE export walk,
# PEB Ldr walk, NtWriteFile loop, frame formatter). Shared by
# backtrace_ops and crash_handler_ops.
# ---------------------------------------------------------------------------

add_object_library(
  bt_format_ops
  SRCS
    debug/bt_format.cpp
  HDRS
    debug/bt_format.h
  DEPENDS
    .ntdll
    libc.src.__support.CPP.span
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
    libc.src.__support.integer_to_string
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# backtrace engine — backtrace, backtrace_symbols, backtrace_symbols_fd.
# Uses the custom stack walker on x64, RtlCaptureStackBackTrace on AArch64.
# ---------------------------------------------------------------------------

add_object_library(
  backtrace_ops
  SRCS
    debug/backtrace.cpp
  HDRS
    debug/backtrace.h
  DEPENDS
    .ntdll
    .bt_format_ops
    .fd_table
    .stack_walker_ops
    libc.src.__support.CPP.string_view
    libc.src.__support.macros.config
    libc.src.stdlib.free
    libc.src.stdlib.malloc
)

# ---------------------------------------------------------------------------
# hw_breakpoint — x86-64 hardware watchpoint/breakpoint API over DR0..DR3.
# Win11 crash-safe cross-thread suspend via NtCreateThreadStateChange.
# ---------------------------------------------------------------------------

add_object_library(
  hw_breakpoint_ops
  SRCS
    debug/hw_breakpoint.cpp
  HDRS
    debug/hw_breakpoint.h
  DEPENDS
    .ntdll
    libc.hdr.errno_macros
    libc.src.__support.macros.config
    libc.src.__support.macros.optimization
)

# ---------------------------------------------------------------------------
# crash handler — async-signal-safe crash backtrace to stderr.
# Called by execute_default_action() for core-dump signals.
# ---------------------------------------------------------------------------

add_object_library(
  crash_handler_ops
  SRCS
    debug/crash_handler.cpp
  HDRS
    debug/crash_handler.h
  DEPENDS
    .ntdll
    .nt_types
    .bt_format_ops
    .stack_walker_ops
    libc.hdr.signal_macros
    libc.src.__support.CPP.span
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
    libc.src.__support.integer_to_string
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Console control handler — Ctrl+C/Break → SIGINT/SIGBREAK delivery.
# ---------------------------------------------------------------------------

# console_ctrl was renamed to console — target kept as alias.
# The actual object library is defined above in the Console / PTY subsystem block.

# ---------------------------------------------------------------------------
# Exec engine — execve/execvp implementation (command line construction,
# env block, fd inheritance, NtCreateUserProcess with exec semantics).
# Extracted from src/unistd/windows/ (Phase 3.20).
# ---------------------------------------------------------------------------

add_object_library(
  exec_ops
  SRCS
    process/exec_ops.cpp
  HDRS
    process/exec_ops.h
  DEPENDS
    .dlfcn_ops
    .env_ops
    .fd_table
    .itimer_ops
    .lazy_init_reset
    .nt_pal
    .ntdll
    .nt_path
    .ntdll
    .pkey_ops
    .process_control_block
    .process_identity
    .process_utils
    .scoped_nt_handle
    .signal_internal_h
    .timer_ops
    libc.hdr.errno_macros
    libc.hdr.fcntl_macros
    libc.hdr.signal_macros
    libc.hdr.types.size_t
    libc.hdr.types.struct_sigaction
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.config
    libc.src.__support.threads.windows.named_semaphore
)

# Generate import libs for the DLLs we use
# ---------------------------------------------------------------------------
# Console / PTY subsystem
# ---------------------------------------------------------------------------

add_header_library(
  terminal_foreground
  HDRS
    process/terminal_foreground.h
  DEPENDS
    .process_control_block
    .signal_h
    .syscall_wrapper_getsid
    .syscall_wrapper_setpgid
    libc.hdr.errno_macros
    libc.hdr.signal_macros
    libc.hdr.types.pid_t
    libc.src.__support.CPP.atomic
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  console_handle_utils
  HDRS
    process/console_handle_utils.h
  DEPENDS
    .condrv
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  pty_shared_state
  HDRS
    process/pty_shared_state.h
  DEPENDS
    libc.hdr.types.pid_t
    libc.hdr.types.struct_winsize
    libc.src.__support.macros.config
)

add_header_library(
  pty_reserved2
  HDRS
    process/pty_reserved2.h
  DEPENDS
    .nt_types
    libc.src.__support.macros.config
)

add_header_library(
  nt_process_utils
  HDRS
    process/nt_process_utils.h
  DEPENDS
    .ntdll
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

add_header_library(
  termios_defaults
  HDRS
    process/termios_defaults.h
  DEPENDS
    .condrv
    libc.src.__support.macros.attributes
    libc.src.__support.macros.config
)

# Note: console_tty uses pty_tree at runtime but the dep is omitted here to
# break the console_tty → pty_tree → getsid → console → console_tty cycle.
# All objects link into the same DLL/archive so the linker resolves it.
add_object_library(
  console_tty
  HDRS
    process/console_tty.h
  SRCS
    process/console_tty_input.cpp
    process/console_tty_output.cpp
    process/console_tty_session.cpp
  DEPENDS
    .condrv
    .console_handle_utils
    .fd_table
    .fork_reinit
    .process_control_block
    .signal_h
    .signal_handler_table_h
    .terminal_foreground
    .termios_defaults
    libc.hdr.errno_macros
    libc.hdr.types.pid_t
    libc.hdr.types.struct_winsize
    libc.src.__support.CPP.atomic
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# Note: pty_tree.cpp forward-declares vt_pty::ensure_adoption to gate
# current_attached_pty_id / has_current_attached_pty on first-use
# inherited-PTY adoption. .vt_pty is not listed in DEPENDS to avoid
# a pty_tree ↔ vt_pty object-library cycle; the symbol is resolved at
# link time from vt_pty.cpp.obj, which is always in the same archive.
add_object_library(
  pty_tree
  HDRS
    process/pty_tree.h
  SRCS
    process/pty_tree.cpp
  DEPENDS
    .console_handle_utils
    .ntdll
    .nt_process_utils
    .nt_types
    .page_alloc
    .pty_reserved2
    .pty_shared_state
    .security
    .signal_internal_h
    .syscall_wrapper_getsid
    .syscall_wrapper_setpgid
    libc.hdr.types.pid_t
    libc.hdr.types.struct_winsize
    libc.src.__support.CPP.scope_guard
    libc.src.__support.error_or
    libc.src.__support.libc_assert
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
)

# Note: console.cpp uses console_tty at runtime but the dep is omitted here
# to break the console ↔ console_tty cycle. Both link into the same archive.
add_object_library(
  console
  HDRS
    process/console.h
  SRCS
    process/console.cpp
  DEPENDS
    .condrv
    .console_handle_utils
    .fd_table
    .lazy_init
    .lazy_init_reset
    .process_utils
    .signal_h
    libc.src.__support.macros.config
    libc.src.setjmp.setjmp
)

add_object_library(
  vt_pty
  HDRS
    process/vt_pty.h
  SRCS
    process/vt_pty.cpp
  DEPENDS
    .condrv
    .console
    .console_handle_utils
    .console_tty
    .fd_table
    .lazy_init
    .lazy_init_reset
    .ntdll
    .nt_process_utils
    .page_alloc
    .process_control_block
    .pty_reserved2
    .pty_shared_state
    .pty_tree
    .syscall_wrapper_getsid
    .syscall_wrapper_setpgid
    .terminal_foreground
    .termios_defaults
    libc.hdr.errno_macros
    libc.hdr.types.pid_t
    libc.hdr.types.struct_winsize
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.new
    libc.src.__support.CPP.scope_guard
    libc.src.__support.CPP.span
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.stringstream
    libc.src.__support.error_or
    libc.src.__support.macros.config
    libc.src.__support.threads.raw_mutex
    libc.src.setjmp.setjmp
)

add_object_library(
  terminal_ops
  HDRS
    process/terminal_ops.h
  SRCS
    process/terminal_ops.cpp
  DEPENDS
    .console_tty
    .fd_table
    .pty_tree
    .terminal_foreground
    .vt_pty
    libc.hdr.types.pid_t
    libc.hdr.types.struct_winsize
    libc.src.__support.error_or
    libc.src.__support.macros.config
)

# ---------------------------------------------------------------------------
# Import libraries
# ---------------------------------------------------------------------------

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_windows_import_lib(ntdll)
  add_windows_import_lib(bcryptprimitives)
  add_windows_import_lib(combase)
  add_windows_import_lib(sspicli)

  add_custom_target(libc-windows-import-libs)
  foreach(import_lib IN ITEMS ntdll sspicli bcryptprimitives combase)
    stage_windows_import_lib(${import_lib})
    add_dependencies(libc-windows-import-libs ${import_lib}_ambient_importlib)
    set(_import_lib "${LIBC_LIBRARY_DIR}/${import_lib}.lib")
    if((WIN32_ITANIUM OR WIN32_NTPOSIX) AND LLVM_LIBRARY_OUTPUT_INTDIR)
      set(_import_lib "${LLVM_LIBRARY_OUTPUT_INTDIR}/${import_lib}.lib")
    endif()
    install(FILES ${_import_lib}
            DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
            COMPONENT libc)
  endforeach()
endif()
