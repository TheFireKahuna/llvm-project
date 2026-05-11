# Layer 2 NBALLOC buddy arena hermetic suite. Drives every public entry
# point (buddy_alloc / buddy_free_sized / buddy_alloc_huge / buddy_free_huge
# / buddy_stats_snapshot) plus multi-thread CAS contention, cross-class
# coalescing, pagemap entry round-trip, generation bump on retire, and
# 1000-cycle stress. Hermetic model so Tier A bring-up runs the full
# memory-primitive registry chain (pal=0, va_substrate=1, mapping_table=2,
# pagemap=3, buddy_arena=4) before main(). 20-test suite mirroring
# pagemap_test.cpp depth.
add_libc_hermetic(
  buddy_arena_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    buddy_arena_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.buddy_arena
    libc.src.__support.OSUtil.windows.nt_pal
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.pagemap
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)

# Layer 7 partition layer hermetic suite. Quality + stress + non-death
# hardening coverage. The PAGE_NOACCESS guard fault tests and the canary-
# tamper trap test require fork/waitpid (death tests in a child process)
# and live in a sibling file `partition_death_test.cpp` that uses
# add_libc_unittest (c.dll runtime; partition symbols NOT exported from
# c.dll today, so death tests are deferred until either the export list
# grows or the canary-trap path can be exercised through a public API).
add_libc_hermetic(
  partition_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    partition_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.app_init
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.partition
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)

add_libc_hermetic(
  partition_min_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    partition_min_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.partition
)

# §9.3.10.1 sub-slab decommit substrate (alloc/slab_chunk_state.h). Pure-
# arithmetic / pure-atomic header — no Tier-A bring-up needed beyond the
# standard hermetic frame. Covers geometry constants, mask round-trip,
# cross-bit isolation, slot-to-sub-page arithmetic for 16 B / 8 KiB /
# 16 KiB / 32 KiB / 64 KiB slot sizes, range-to-mask conversion,
# subpage_address_for offsets, and compute_idle_subpages boundaries.
add_libc_hermetic(
  slab_chunk_state_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    slab_chunk_state_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.OSUtil.windows.alloc_slab_chunk_state
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.ntdll
)
