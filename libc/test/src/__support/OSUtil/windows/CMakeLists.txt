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

add_libc_test(
  sparse_radix_pointer_index_test
  SUITE
    libc-osutil-tests
  SRCS
    sparse_radix_pointer_index_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.sparse_radix_pointer_index
  LINK_LIBRARIES
    windows_ntdll
)

add_libc_test(
  concurrent_pointer_registry_test
  SUITE
    libc-osutil-tests
  SRCS
    concurrent_pointer_registry_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.concurrent_pointer_registry
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
    libc.test.UnitTest.Test
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
    libc.test.UnitTest.Test
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
    libc.test.UnitTest.Test
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
