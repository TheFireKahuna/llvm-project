# Memory subsystem hermetic tests — drive g_region_pool / g_mapping_table /
# region_reconcile directly, mostly via the public mmap surface. Each test
# below establishes a load-bearing invariant of the new region-and-shape
# design (alloc_id ABA, MONO->CHUNKED promotion, FOREIGN cordon, etc.).
add_subdirectory(alloc)
add_subdirectory(memory)

add_libc_test(
  temp_path_test
  SUITE
    libc-osutil-tests
  SRCS
    temp_path_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.temp_path
  LINK_LIBRARIES
    windows_ntdll
)

# ThreadLocalWord full primitive coverage. Every owner-visible entry
# point and every cross-thread static, plus the no-ghost-wake regression
# for the alert-drain path.
add_libc_unittest(
  thread_local_word_test
  SUITE
    libc-osutil-tests
  SRCS
    thread_local_word_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.threads.windows.futex_utils
    libc.src.__support.threads.windows.thread_local_word
    libc.src.__support.time.abs_timeout
)

# Lock-free linkage substrate — pure-substrate coverage against a mock
# intrusive node. Verifies tag monotonicity, bit preservation tables,
# every CAS / mark / certify primitive, and a 4-thread ABA-defense
# soak. No dependency on wait_slot's pool — node memory is a static
# array (Safety Triad T3 honored).
add_libc_unittest(
  lock_free_linkage_test
  SUITE
    libc-osutil-tests
  SRCS
    lock_free_linkage_test.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.lock_free_linkage
    libc.src.__support.OSUtil.windows.ntdll
)

# Layer 0 PAL (`nt_pal::`) round-trip coverage — process-cookie + large
# pages probes are deterministic, placeholder reserve / commit_replace /
# free_placeholder go through cleanly, and the Phase 1 invariant
# (commit_replace ALWAYS adds MEM_WRITE_WATCH) is observable via a
# write-watch query. offer/reclaim is exercised end-to-end against the
# MEM_RESET / MEM_RESET_UNDO syscalls.
add_libc_unittest(
  nt_pal_test
  SUITE
    libc-osutil-tests
  SRCS
    nt_pal_test.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.OSUtil.windows.nt_pal
    libc.src.__support.OSUtil.windows.ntdll
)

# Layer 2 pagemap (alloc::pagemap) hermetic test — drives every public
# entry point single-threaded, then concurrent torture targeting the
# load-bearing race windows: hot/cold half ordering, retire-CAS
# generation ABA defense, refcounted L1 commit-state machine, leak-
# freeness of register/unregister cycles. Hermetic model so the pagemap
# is fully booted by the time main() runs (LIBC_REGISTER_MEMORY_PRIMITIVE
# phase 3 fires before __libc_init dispatches to main).
add_libc_hermetic(
  pagemap_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    pagemap_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.__support.OSUtil.windows.nt_pal
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.pagemap
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
)

# Inner concurrent interval skiplist (Kim et al. SOSP 2025) — Lock /
# Swap / Query / Alloc / Map coverage plus 4-thread Herlihy-Wing fuzz
# skeleton. Verifies paper invariants I1..I7 plus Theorem B.2 (Lock-time
# non-INVALIDATED gate) under contention, the substrate Safety Triad
# composition, and chunk-bitmap allocator reclamation.
add_libc_unittest(
  interval_skiplist_test
  SUITE
    libc-osutil-tests
  SRCS
    interval_skiplist_test.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.interval_skiplist
    libc.src.__support.OSUtil.windows.lock_free_linkage
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.skiplist_link_traits
    libc.src.__support.OSUtil.windows.va_region_desc
)

# va_tracker public typed-ops end-to-end tests. Exercises the full ART +
# interval skiplist + RegionDesc partition stack composed under
# `memory/va_tracker.{h,cpp}`. Covers acquire / release / replace /
# mutate / split / resolve / walk_range, plus serialize_for_fork +
# replay_in_child round-trip and per-resolve published-clone visibility
# after mutate.
add_libc_unittest(
  va_tracker_test
  SUITE
    libc-osutil-tests
  SRCS
    va_tracker_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.va_region_desc
    libc.src.__support.OSUtil.windows.va_tracker
)

# Property-based linearizability fuzzer for the ART × interval-skiplist
# × Crystalline-W composition. Mitigates the §17.3 risk #1 named in
# `NTPOSIX_MEMORY_ARCHITECTURE_DESIGN.md` and gates Phase 3 cutover per
# `ROADMAP.md`. Built on the generic `concurrent_fuzz` framework
# (history-recording + sequential-reference replay + Wing-Gibbons
# linearizability checker + delta-debugging shrinker).
add_libc_unittest(
  interval_skiplist_lin_fuzz_test
  SUITE
    libc-osutil-tests
  SRCS
    interval_skiplist_lin_fuzz_test.cpp
  DEPENDS
    libc.hdr.errno_macros
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.crystalline_registry
    libc.src.__support.OSUtil.windows.va_tracker
    libc.src.__support.OSUtil.windows.va_region_desc
    libc.test.src.__support.concurrent_fuzz.concurrent_fuzz
  LINK_LIBRARIES
    windows_ntdll
)

# SlabPool hardening death tests. Each scenario forks and observes the
# child failing via WIFSIGNALED — if the hardening regressed into
# non-trapping behavior, the child would return a sentinel exit code.
add_libc_unittest(
  slab_pool_death_test
  SUITE
    libc-osutil-tests
  SRCS
    slab_pool_death_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.stdlib.free
    libc.src.stdlib.malloc
    libc.src.sys.wait.waitpid
    libc.src.unistd.fork
)

# publish_handles rollback regression test — internal-primitive test,
# hermetic link (no c.dll). Exercises internal::console::publish_handles /
# restore_handles directly against a private PCB/fd-table instance, same
# model as futex_bench / thread_ring_bench.
add_libc_hermetic(
  publish_handles_rollback_test
  SUITE
    libc-osutil-tests
  SRCS
    publish_handles_rollback_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.OSUtil.windows.console
    libc.src.__support.OSUtil.windows.condrv
    libc.src.__support.OSUtil.windows.nt_process
)

# ===----------------------------------------------------------------------===
# PTY / console / slab-pool integration tests — exercise POSIX APIs end-to-end.
# Hermetic model (futex_bench-style): each test gets its own isolated
# `.__libc__` archive assembled from OBJECT_FILE_RAW of every entrypoint in
# DEPENDS, plus crt1 / hermetic_runtime. No c.dll, no LibcTest.unit. Tests
# that use TEST() macros link LibcTest.hermetic; helper exes carry their own
# `extern "C" int main()` and skip the framework entirely, like
# thread_ring_bench.
# ===----------------------------------------------------------------------===

# --- devpts smoke test ---
add_libc_hermetic(
  vt_pty_devpts_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    vt_pty_devpts_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.fcntl.openat
    libc.src.stdlib.grantpt
    libc.src.stdlib.ptsname_r
    libc.src.stdlib.unlockpt
    libc.src.string.strlen
    libc.src.string.strncmp
    libc.src.unistd.close
)

# --- Tree-scoped PTY join: child helper + parent test ---
# Child helper is a standalone exe with its own main — no test framework.
add_libc_hermetic(
  vt_pty_tree_join_child
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    vt_pty_tree_join_child.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.stdio.fprintf
    libc.src.stdio.stderr
    libc.src.sys.ioctl.ioctl
    libc.src.termios.tcgetattr
    libc.src.unistd.close
    libc.src.unistd.isatty
    libc.src.unistd.read
)

add_libc_hermetic(
  vt_pty_tree_join_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    vt_pty_tree_join_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.errno.errno
    libc.src.fcntl.posix_openpt
    libc.src.signal.signal
    libc.src.spawn.posix_spawn
    libc.src.spawn.posix_spawn_file_actions_addclose
    libc.src.spawn.posix_spawn_file_actions_adddup2
    libc.src.spawn.posix_spawn_file_actions_destroy
    libc.src.spawn.posix_spawn_file_actions_init
    libc.src.stdlib.grantpt
    libc.src.stdlib.ptsname_r
    libc.src.stdlib.unlockpt
    libc.src.string.strlen
    libc.src.sys.wait.waitpid
    libc.src.unistd.close
    libc.src.unistd.pipe
    libc.src.unistd.write
)
get_fq_target_name(vt_pty_tree_join_test _vt_pty_tree_join_test_fq)
get_fq_target_name(vt_pty_tree_join_child _vt_pty_tree_join_child_fq)
if(TARGET ${_vt_pty_tree_join_test_fq})
  add_dependencies(${_vt_pty_tree_join_test_fq}
                   ${_vt_pty_tree_join_child_fq})
  target_compile_definitions(${_vt_pty_tree_join_test_fq} PRIVATE
    VT_PTY_TREE_JOIN_CHILD_PATH="$<TARGET_FILE:${_vt_pty_tree_join_child_fq}>"
  )
endif()

# --- Console TTY behavior tests ---
add_libc_hermetic(
  console_tty_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    console_tty_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.errno.errno
    libc.src.fcntl.fcntl
    libc.src.pty.openpty
    libc.src.signal.kill
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
    libc.src.signal.signal
    libc.src.sys.ioctl.ioctl
    libc.src.termios.tcdrain
    libc.src.termios.tcflow
    libc.src.termios.tcflush
    libc.src.termios.tcgetattr
    libc.src.termios.tcsendbreak
    libc.src.termios.tcsetattr
    libc.src.unistd.close
    libc.src.unistd.getpgrp
    libc.src.unistd.isatty
    libc.src.unistd.pipe
    libc.src.unistd.read
    libc.src.unistd.tcgetpgrp
    libc.src.unistd.tcsetpgrp
    libc.src.unistd.usleep
    libc.src.unistd.write
)

# --- PTY lifecycle integration tests ---
add_libc_hermetic(
  pty_lifecycle_child
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    pty_lifecycle_child.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.stdio.fprintf
    libc.src.stdio.stderr
    libc.src.sys.ioctl.ioctl
    libc.src.termios.tcgetattr
    libc.src.unistd.close
    libc.src.unistd.isatty
    libc.src.unistd.read
    libc.src.unistd.write
)

add_libc_hermetic(
  pty_lifecycle_test
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    pty_lifecycle_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.errno.errno
    libc.src.fcntl.fcntl
    libc.src.fcntl.open
    libc.src.fcntl.posix_openpt
    libc.src.pty.openpty
    libc.src.signal.signal
    libc.src.spawn.posix_spawn
    libc.src.spawn.posix_spawn_file_actions_addclose
    libc.src.spawn.posix_spawn_file_actions_adddup2
    libc.src.spawn.posix_spawn_file_actions_destroy
    libc.src.spawn.posix_spawn_file_actions_init
    libc.src.stdlib.grantpt
    libc.src.stdlib.ptsname_r
    libc.src.stdlib.unlockpt
    libc.src.string.memcmp
    libc.src.string.strlen
    libc.src.sys.ioctl.ioctl
    libc.src.sys.wait.waitpid
    libc.src.termios.tcgetattr
    libc.src.termios.tcsetattr
    libc.src.unistd.close
    libc.src.unistd.fork
    libc.src.unistd.isatty
    libc.src.unistd.pipe
    libc.src.unistd.read
    libc.src.unistd.setsid
    libc.src.unistd.usleep
    libc.src.unistd.write
)
get_fq_target_name(pty_lifecycle_test _pty_lifecycle_test_fq)
get_fq_target_name(pty_lifecycle_child _pty_lifecycle_child_fq)
if(TARGET ${_pty_lifecycle_test_fq})
  add_dependencies(${_pty_lifecycle_test_fq}
                   ${_pty_lifecycle_child_fq})
  target_compile_definitions(${_pty_lifecycle_test_fq} PRIVATE
    PTY_LIFECYCLE_CHILD_PATH="$<TARGET_FILE:${_pty_lifecycle_child_fq}>"
  )
endif()

# --- SlabPool / posix_alloc aggressive stress ---
# Standalone exe with own main — hermetic, no test framework. Run manually
# to reproduce allocator races observed in stage-2 builds.
add_libc_hermetic(
  slab_pool_stress
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    slab_pool_stress.cpp
  DEPENDS
    libc.src.errno.errno
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
    libc.src.signal.raise
    libc.src.signal.sigaction
    libc.src.signal.signal
    libc.src.stdio.fflush
    libc.src.stdio.fprintf
    libc.src.stdlib.abort
    libc.src.stdlib.atoi
    libc.src.stdlib.free
    libc.src.stdlib.getenv
    libc.src.stdlib.malloc
    libc.src.stdlib.realloc
    libc.src.stdlib.strtoul
    libc.src.string.memcpy
    libc.src.string.strcmp
    libc.src.sys.mman.mmap
    libc.src.time.nanosleep
    libc.src.unistd.write
)

# --- Slot-pool chain corruption stress test ---
# Exercises CrystallineSlotPool::claim_slot / release_slot (the Treiber +
# Harris-marked chain primitives the SMR pool builds on) in isolation —
# this is NOT a Crystalline-W SMR test; that coverage lives in
# crystalline_domain_stress.cpp. Hermetic so we stress an isolated
# CrystallineSlotPool, not c.dll's g_registry_domain.pool. Owns its main
# (no test framework) so scenarios are env-var configurable: CCS_THREADS,
# CCS_ITERS, CCS_SCENARIO (1=churn, 2=bulk, 3=spmc), CCS_HOLDINGS.
add_libc_hermetic(
  crystalline_slot_pool_stress
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    crystalline_slot_pool_stress.cpp
  COMPILE_OPTIONS
    -DLIBC_CRYSTALLINE_SPLICE_TRACE=1
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.crystalline_local_state
    libc.src.__support.OSUtil.windows.lock_free_linkage
    libc.src.__support.OSUtil.windows.page_alloc
    libc.src.errno.errno
    libc.src.pthread.pthread_create
    libc.src.pthread.pthread_join
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
    libc.src.stdlib._Exit
    libc.src.stdlib.getenv
)

# --- ThreadRing / BatchEngineT microbenchmark ---
# Internal-primitive bench: exercises ThreadRing / BatchEngineT directly via
# their LIBC_NAMESPACE APIs. Hermetic link — no c.dll or c.lib involvement.
# Per-test archive carries RAW (mangled) objects for every DEPENDS entry.
add_libc_hermetic(
  thread_ring_bench
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    thread_ring_bench.cpp
  COMPILE_OPTIONS
    -fno-stack-protector
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.CPP.scope_guard
    libc.src.__support.OSUtil.windows.io_ring_helpers
    libc.src.__support.OSUtil.windows.thread_ring
    libc.src.__support.OSUtil.windows.nt_process_api
    libc.src.__support.OSUtil.windows.ntdll
)

# --- Init/fini cleanup test battery ---
# Hermetic unit tests for PCB zone seals, VEH dispatch, the tri-state init
# gate, .libcfin phased teardown, LazyInit FaultGuard semantics, and fork
# canary validation, plus cdll end-to-end witnesses (SIGSEGV routing, DLL
# load/free handle and TLS-bitmap leaks, fork→spawn→exec reinit pipeline).
add_subdirectory(startup)
