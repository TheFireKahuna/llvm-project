# =============================================================================
# Windows memory subsystem hermetic tests.
#
# Each target below pins one load-bearing invariant of the redesign described
# at ~/.claude/plans/humble-roaming-lampson.md — RegionDesc/RegionPool, the
# alloc_id ABA defence, the seqlock+resolve protocol, MONO->CHUNKED shape
# promotion, FOREIGN cordon, and the recipe budgets pinned per logical mmap
# call. Tests reach directly into LIBC_NAMESPACE::windows::memory globals
# (g_region_pool, g_mapping_table) under MmapLock to assert internal state
# consistent with externally-visible behaviour.
# =============================================================================

# Recipe-budget invariants — every logical mmap takes the documented number
# of region acquires/releases (anon=1, file=1+section, partial-anon-munmap=0
# new region, etc.). Catches regressions that re-introduce MBI classify
# loops or extra register_mapping calls.
add_libc_unittest(
  mmap_syscall_count_test
  SUITE
    libc-osutil-tests
  SRCS
    mmap_syscall_count_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_mman_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.fcntl
    libc.src.__support.OSUtil.windows.file_ops
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.region_pool
    libc.src.errno.errno
    libc.src.sys.mman.memfd_create
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.unistd.close
    libc.src.unistd.ftruncate
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

# RegionPool acquire/release/refcount/fork_reinit contract. Drives the pool
# directly with synthetic AcquireSpec values; asserts alloc_id monotonicity
# under slot recycle and the resolve()/resolve_unlocked() guard.
add_libc_unittest(
  region_pool_lifecycle_test
  SUITE
    libc-osutil-tests
  SRCS
    region_pool_lifecycle_test.cpp
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.region_pool
)

# FILE_VIEW_MONO -> FILE_VIEW_CHUNKED promotion under partial-munmap, plus
# the per-region chunk_list_lock parking-mutex contention path. Drives the
# shape transition through the public mmap/munmap surface and reads the
# resulting RegionDesc state via g_mapping_table.snapshot().
add_libc_unittest(
  region_shape_transition_test
  SUITE
    libc-osutil-tests
  SRCS
    region_shape_transition_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_mman_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.fcntl
    libc.src.__support.OSUtil.windows.file_ops
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.region_pool
    libc.src.errno.errno
    libc.src.sys.mman.memfd_create
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.unistd.close
    libc.src.unistd.ftruncate
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

# FOREIGN cordon protocol — direct NtAllocateVirtualMemoryEx placeholder,
# cordon_foreigners_in_range, hint avoidance, MAP_FIXED_NOREPLACE EEXIST,
# revalidation on free, state_aux probe-lock single-flight under contention.
add_libc_unittest(
  region_foreign_cordon_test
  SUITE
    libc-osutil-tests
  SRCS
    region_foreign_cordon_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_mman_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.page_size
    libc.src.__support.OSUtil.windows.region_pool
    libc.src.__support.OSUtil.windows.region_reconcile
    libc.src.__support.threads.windows.spin_wait
    libc.src.errno.errno
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

# RemapTransaction::commit() now returns bool with a real rollback path on
# register_mapping failure (regression for the OLD silent-drop at line 147).
# Pins the success-path contract across head/middle/tail partial-unmap
# orientations; rollback case is DISABLED_* pending a fault-injection seam.
add_libc_unittest(
  remap_transaction_register_failure_test
  SUITE
    libc-osutil-tests
  SRCS
    remap_transaction_register_failure_test.cpp
  DEPENDS
    libc.hdr.types.off_t
    libc.include.llvm-libc-macros.sys_mman_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.fcntl
    libc.src.__support.OSUtil.windows.file_ops
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.region_pool
    libc.src.errno.errno
    libc.src.sys.mman.memfd_create
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.unistd.close
    libc.src.unistd.ftruncate
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)

# Seqlock snapshot + post-fence RegionPool::resolve() ABA defence under
# concurrent recycle. The single test that pressures the (region_id,
# alloc_id) tuple at the resolve boundary; a reader that fails to retry on
# alloc_id mismatch would silently observe a different region.
add_libc_unittest(
  seqlock_region_resolve_test
  SUITE
    libc-osutil-tests
  SRCS
    seqlock_region_resolve_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_mman_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.mapping_table
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.region_pool
    libc.src.errno.errno
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.test.UnitTest.ErrnoCheckingTest
    libc.test.UnitTest.ErrnoSetterMatcher
)
