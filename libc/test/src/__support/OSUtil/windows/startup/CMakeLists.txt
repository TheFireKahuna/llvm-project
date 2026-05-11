# =============================================================================
# Init/fini cleanup test battery.
#
# Each target below pins one load-bearing invariant of the Tier A / Tier B
# bring-up and FreeLibrary teardown contracts:
#
#   - PCB Zone 0 / 0b page seal alignment and hardware immutability.
#   - The tri-state init gate (UNINIT → INITIALIZING → READY/POISONED),
#     partial-failure rollback, and poisoned-gate re-attach rejection.
#   - `.libcfin` phased walker ordering and intra-phase no-futex invariant.
#   - Master VEH filter-table dispatch order and Zone 0 sealing.
#   - Fork reinit canary validation before cookie rotation.
#   - LazyInit FaultGuard "InitFn fault → NtTerminateProcess" semantics.
#
# Split between add_libc_hermetic and add_libc_unittest follows whether the
# test needs the live process-wide libc state:
#
#   - Hermetic (local state / mocks, no live g_pcb or real `.libcfin` walk):
#     bootstrap gate race, partial-failure mock pipeline, poisoned-gate
#     5-state machine, phased-walker private `.libcfintest` probe.
#
#   - Unittest (real live libc state, fork+waitpid death tests):
#     PCB layout & seals, VEH Zone 0 dispatch, real `.libcfin$P4` probe,
#     fork_reinit canary corruption, LazyInit FaultGuard under real VEH.
#
# The end-to-end tests (sigsegv, fork+segv, dlopen, ctor-faults, cdll
# resource-balance, fork→spawn→exec) use add_libc_hermetic for the
# parent side where possible. A handful require a freestanding child
# exe or DLL that is NOT statically linked to c.dll (otherwise the
# loader pins c.dll for the helper's lifetime and FreeLibrary becomes
# a no-op); those are built below as plain CMake primitives.
# =============================================================================

# -----------------------------------------------------------------------------
# Hermetic tests (LibcTest.hermetic) — local state, per-test private libc
# archive. These tests do not touch the real process-wide g_pcb, real
# `.libcfin` section, or real VEH dispatch — they either build the same
# state machine in a template or use a private parallel section.
# -----------------------------------------------------------------------------

# Tri-state gate concurrent bootstrap race (§1). Reproduces the
# __libc_init vs __libc_dll_init CAS collision in a local TriStateGate
# template under high-contention 2T and 8T workloads + poisoned-gate
# rejection. Does not touch g_subsystems_up.
add_libc_hermetic(
  bootstrap_then_dll_init_concurrent_test
  SUITE
    libc-osutil-tests
  SRCS
    bootstrap_then_dll_init_concurrent_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.macros.config
)

# Tri-state gate partial-init rollback. MockPipeline with N subsystems;
# asserts exactly phases 1..K-1 init, reverse 1..K-1 fini on failure at
# phase K, and gate stays POISONED across re-attach attempts. No live
# subsystem state involved.
add_libc_hermetic(
  init_partial_failure_runs_fini_test
  SUITE
    libc-osutil-tests
  SRCS
    init_partial_failure_runs_fini_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
)

# Tri-state gate POISONED terminal-state coverage. 8T × 1000-iter
# adversarial model of the 5-state CAS machine; every observed
# transition must be a member of the allowed-edge set.
add_libc_hermetic(
  init_gate_poison_then_reattach_test
  SUITE
    libc-osutil-tests
  SRCS
    init_gate_poison_then_reattach_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
)

# `.libcfin` phased walker ordering. Private `.libcfintest` section with
# deliberately non-consecutive phase registrations so empty-bucket skips
# and reverse iteration are exercised. Real `.libcfin` is untouched.
add_libc_hermetic(
  fini_registry_phase_order_walk_test
  SUITE
    libc-osutil-tests
  SRCS
    fini_registry_phase_order_walk_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.__support.OSUtil.windows.section_registry
    libc.src.__support.macros.config
)

# -----------------------------------------------------------------------------
# Unit tests (LibcTest.unit) — real process-wide libc state. These tests
# observe or perturb the live g_pcb, real `.libcfin$P4` section, real
# VEH filter table, or real fork/FaultGuard dispatch. Most are
# fork+waitpid death tests in the slab_pool_death_test mould.
# -----------------------------------------------------------------------------

# PCB layout — zone alignment & page fit. Compile-time static_asserts plus
# runtime witnesses on the linker-assigned g_pcb address.
add_libc_unittest(
  pcb_zone_alignment_test
  SUITE
    libc-osutil-tests
  SRCS
    pcb_zone_alignment_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.__support.macros.config
)

# PCB Zone 0 + Zone 0b PAGE_READONLY seal. Fork-based death tests: child
# writes into each sealed zone and must AV with SIGSEGV.
add_libc_unittest(
  pcb_zone0_seal_av_test
  SUITE
    libc-osutil-tests
  SRCS
    pcb_zone0_seal_av_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.sys.wait.waitpid
    libc.src.unistd.fork
)

# Master VEH Zone 0 sealing + dispatch-state liveness. Fork-based death
# tests write each VehSealedState field in the real g_pcb; parent asserts
# SIGSEGV. Also pins `veh_sealed()` const accessor (no runtime mutator API).
add_libc_unittest(
  veh_zone0_sealed_after_bootstrap_test
  SUITE
    libc-osutil-tests
  SRCS
    veh_zone0_sealed_after_bootstrap_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.__support.CPP.type_traits
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.sys.wait.waitpid
    libc.src.unistd.fork
)

# Master VEH priority-sorted filter-table dispatch. Exercises the real
# `.libcveh` dispatch loop: filters are invoked in strict priority order
# and CONTINUE_SEARCH propagation never skips a rank.
add_libc_unittest(
  veh_dispatch_priority_order_test
  SUITE
    libc-osutil-tests
  SRCS
    veh_dispatch_priority_order_test.cpp
  DEPENDS
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.__support.OSUtil.windows.veh_core
    libc.src.__support.OSUtil.windows.veh_filter_registry
)

# Fork reinit canary pre-rotate validation. Parent corrupts Zone 0b
# canary in the real g_pcb via PcbInitAccess, forks, child's
# libc_fork_reinit MUST detect the mismatch and NtTerminateProcess(127)
# BEFORE trusting the cookie.
add_libc_unittest(
  fork_reinit_canary_validates_pre_rotate_test
  SUITE
    libc-osutil-tests
  SRCS
    fork_reinit_canary_validates_pre_rotate_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    libc.src.sys.wait.waitpid
    libc.src.unistd.fork
)

# LazyInit FaultGuard termination semantics. Fork-based death test:
# child's InitFn AVs under the real master VEH, FaultGuard traps and
# NtTerminateProcess with STATUS_DLL_INIT_FAILED — parent asserts
# WIFEXITED + encoded exit. Companion happy-path test pins single-flight
# CAS behaviour.
add_libc_unittest(
  lazy_init_initfn_fault_terminates_test
  SUITE
    libc-osutil-tests
  SRCS
    lazy_init_initfn_fault_terminates_test.cpp
  DEPENDS
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.lazy_init
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.sys.wait.waitpid
    libc.src.unistd.fork
)

# -----------------------------------------------------------------------------
# End-to-end hermetic tests (LibcTest.hermetic + crt1 + hermetic_runtime).
# The Windows hermetic runtime statically links libc.startup.windows.crt1 into
# the test exe and wires the Itanium ABI floor via hermetic_runtime — so
# Tier A / Tier B bring-up runs in-process exactly as it would inside c.dll.
# These tests exercise POSIX surface (sigaction, fork, dlopen, posix_spawn)
# against a fully-booted in-process libc.
#
# One exception: libc_dll_cycle_resource_balance_test needs a real
# __libc_dll_init / __libc_dll_fini pair driven by DllMain, which is
# only observable from a process that does NOT statically link c.dll.
# Its child helper is built below as a freestanding ntdll-only exe.
# -----------------------------------------------------------------------------

# Null-deref → SIGSEGV delivery. Validates that the master VEH translates
# EXCEPTION_ACCESS_VIOLATION into a POSIX SIGSEGV and the SA_SIGINFO
# handler sees the expected siginfo fields. Companion no-handler case
# forks and asserts WIFSIGNALED + WTERMSIG == SIGSEGV.
add_libc_hermetic(
  sigsegv_from_null_deref_handled_test
  SUITE
    libc-osutil-tests
  SRCS
    sigsegv_from_null_deref_handled_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.errno.errno
    libc.src.setjmp.sigsetjmp
    libc.src.setjmp.siglongjmp
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
    libc.src.string.strerror
    libc.src.sys.wait.waitpid
    libc.src.unistd._exit
    libc.src.unistd.close
    libc.src.unistd.fork
    libc.src.unistd.pipe
    libc.src.unistd.read
    libc.src.unistd.write
)

# Post-fork SIGSEGV delivery. Validates that veh_core_fork_reinit
# re-registered the master VEH in the child after RtlCloneUserProcess —
# the child's null-deref must still dispatch through the libc's VEH and
# be delivered as SIGSEGV (either to the inherited handler or as default
# termination), not escape to the system unhandled-exception filter.
add_libc_hermetic(
  fork_then_segv_in_child_test
  SUITE
    libc-osutil-tests
  SRCS
    fork_then_segv_in_child_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.errno.errno
    libc.src.setjmp.sigsetjmp
    libc.src.setjmp.siglongjmp
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
    libc.src.stdlib._Exit
    libc.src.sys.wait.waitpid
    libc.src.unistd._exit
    libc.src.unistd.fork
)

# dlopen'd DLL → fault → master VEH. Validates that the DLL-load
# notification re-registers the master VEH at front-of-chain so a fault
# inside a newly loaded third-party DLL still delivers via the libc.
# Depends on the dlopen_segv_helper DLL built below.
add_libc_hermetic(
  dlopen_then_segv_test
  SUITE
    libc-osutil-tests
  SRCS
    dlopen_then_segv_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.src.dlfcn.dlclose
    libc.src.dlfcn.dlerror
    libc.src.dlfcn.dlopen
    libc.src.dlfcn.dlsym
    libc.src.errno.errno
    libc.src.setjmp.sigsetjmp
    libc.src.setjmp.siglongjmp
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
)

# Faulting static ctor under loader lock. Validates that the master VEH
# (re-registered front-of-chain by dll_notify_callback on
# LDR_DLL_NOTIFICATION_REASON_LOADED) owns a fault raised from a third-
# party DLL's DLL_PROCESS_ATTACH ctor while the loader lock is held.
# Depends on the ctor_fault_helper DLL built below.
add_libc_hermetic(
  ctor_faults_during_startup_test
  SUITE
    libc-osutil-tests
  SRCS
    ctor_faults_during_startup_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.dlfcn.dlopen
    libc.src.dlfcn.dlsym
    libc.src.signal.sigaction
    libc.src.stdlib.setenv
    libc.src.sys.wait.waitpid
    libc.src.unistd._exit
    libc.src.unistd.fork
)

# Full reinit pipeline: fork → posix_spawn → exec_self_hollow.
# Exercises libc_fork_reinit (zone 0b canary rotate, veh re-register,
# memory_reconcile_*, fls_fork_reinit, lifecycle fixup), posix_spawn
# Reserved2Ext attribute passing, and PID-preserving exec_self_hollow.
# Depends on fork_then_spawn_then_exec_helper (hermetic exe, built below).
add_libc_hermetic(
  fork_then_spawn_then_exec_test
  SUITE
    libc-osutil-tests
  SRCS
    fork_then_spawn_then_exec_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.include.llvm-libc-macros.signal_macros
    libc.include.llvm-libc-macros.sys_wait_macros
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.signal.sigaction
    libc.src.signal.signal
    libc.src.spawn.posix_spawn
    libc.src.spawn.posix_spawn_file_actions_addclose
    libc.src.spawn.posix_spawn_file_actions_adddup2
    libc.src.spawn.posix_spawn_file_actions_destroy
    libc.src.spawn.posix_spawn_file_actions_init
    libc.src.string.strerror
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.sys.wait.waitpid
    libc.src.unistd._exit
    libc.src.unistd.close
    libc.src.unistd.fork
    libc.src.unistd.getpid
    libc.src.unistd.pipe
    libc.src.unistd.read
    libc.src.unistd.write
)

# Exec target for fork_then_spawn_then_exec_test. Hermetic exe with its
# own `int main` — the parent test passes its path via a preprocessor
# define resolved from $<TARGET_FILE:...>. NO_RUN_POSTBUILD skips the
# test-harness wrapper (this is a plain program, not a TEST() binary).
add_libc_hermetic(
  fork_then_spawn_then_exec_helper
  SUITE
    libc-osutil-tests
  NO_RUN_POSTBUILD
  SRCS
    fork_then_spawn_then_exec_helper.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.brk_state
    libc.src.errno.errno
    libc.src.fcntl.open
    libc.src.signal.raise
    libc.src.signal.sigaction
    libc.src.signal.sigemptyset
    libc.src.stdio.fprintf
    libc.src.stdio.snprintf
    libc.src.stdio.stderr
    libc.src.stdlib.atoi
    libc.src.stdlib.exit
    libc.src.stdlib.strtoll
    libc.src.string.strcmp
    libc.src.string.strerror
    libc.src.string.strlen
    libc.src.sys.mman.mmap
    libc.src.sys.mman.munmap
    libc.src.unistd.close
    libc.src.unistd.execv
    libc.src.unistd.getpid
    libc.src.unistd.read
    libc.src.unistd.write
)

# Parent test needs the helper's on-disk path. Match the macro name
# defined at the top of fork_then_spawn_then_exec_test.cpp.
get_fq_target_name(fork_then_spawn_then_exec_test
                   fork_then_spawn_then_exec_test_target)
get_fq_target_name(fork_then_spawn_then_exec_helper
                   fork_then_spawn_then_exec_helper_target)
target_compile_definitions(
  ${fork_then_spawn_then_exec_test_target}.__build__
  PRIVATE
    "FORK_SPAWN_EXEC_HELPER_PATH=\"$<TARGET_FILE:${fork_then_spawn_then_exec_helper_target}>\""
)
add_dependencies(${fork_then_spawn_then_exec_test_target}.__build__
                 ${fork_then_spawn_then_exec_helper_target})

# -----------------------------------------------------------------------------
# Third-party-style helper DLLs for the dlopen + ctor-faults tests.
#
# These are NOT libc tests — they are bare SHARED libraries that simulate a
# random third-party DLL whose ctor or exported function faults. They must
# stay freestanding (no libc link) so the resulting EXCEPTION_ACCESS_VIOLATION
# is unambiguously caused by their own code, not by libc init failures.
# Plain CMake primitives (add_library SHARED + ntdll only); no macro is
# appropriate because these aren't tests.
# -----------------------------------------------------------------------------

add_library(dlopen_segv_helper SHARED EXCLUDE_FROM_ALL
            dlopen_segv_helper.cpp)
target_link_libraries(dlopen_segv_helper PRIVATE windows_ntdll)
target_link_options(dlopen_segv_helper PRIVATE
                    -nolibc -nostartfiles -nostdlib++ -unwindlib=none
                    "LINKER:/entry:DllMain")
set_target_properties(dlopen_segv_helper PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(libc-osutil-tests dlopen_segv_helper)
get_fq_target_name(dlopen_then_segv_test dlopen_then_segv_test_target)
add_dependencies(${dlopen_then_segv_test_target}.__build__ dlopen_segv_helper)
target_compile_definitions(
  ${dlopen_then_segv_test_target}.__build__
  PRIVATE
    DLOPEN_SEGV_HELPER_PATH="$<TARGET_FILE:dlopen_segv_helper>"
)

add_library(ctor_fault_helper SHARED EXCLUDE_FROM_ALL
            ctor_fault_helper.cpp)
target_link_libraries(ctor_fault_helper PRIVATE windows_ntdll)
target_link_options(ctor_fault_helper PRIVATE
                    -nolibc -nostartfiles -nostdlib++ -unwindlib=none
                    "LINKER:/entry:DllMain")
set_target_properties(ctor_fault_helper PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(libc-osutil-tests ctor_fault_helper)
get_fq_target_name(ctor_faults_during_startup_test
                   ctor_faults_during_startup_test_target)
add_dependencies(${ctor_faults_during_startup_test_target}.__build__
                 ctor_fault_helper)
target_compile_definitions(
  ${ctor_faults_during_startup_test_target}.__build__
  PRIVATE
    CTOR_FAULT_HELPER_PATH="$<TARGET_FILE:ctor_fault_helper>"
)

# -----------------------------------------------------------------------------
# c.dll load/free resource-balance test + freestanding child.
#
# Parent is a standard hermetic test. Child is an ntdll-only exe that
# LoadLibrary's c.dll, FreeLibrary's it, and reports TLS-bitmap + handle
# deltas via an exit-code bitmask. Child MUST NOT link c.lib — if c.dll
# lands on its IAT the loader pins it for process lifetime, and the
# explicit FreeLibrary becomes a no-op (the whole point of the test
# evaporates). Hence a plain add_executable with windows_ntdll only.
# -----------------------------------------------------------------------------

add_libc_hermetic(
  libc_dll_cycle_resource_balance_test
  SUITE
    libc-osutil-tests
  SRCS
    libc_dll_cycle_resource_balance_test.cpp
  LINK_LIBRARIES
    LibcTest.hermetic
  DEPENDS
    libc.src.spawn.posix_spawn
    libc.src.sys.wait.waitpid
)

add_executable(libc_dll_cycle_resource_balance_child EXCLUDE_FROM_ALL
               libc_dll_cycle_resource_balance_child.cpp)
target_link_libraries(libc_dll_cycle_resource_balance_child
                      PRIVATE windows_ntdll)
target_link_options(libc_dll_cycle_resource_balance_child PRIVATE
                    -nolibc -nostartfiles -nostdlib++ -unwindlib=none
                    "LINKER:/entry:main")
set_target_properties(libc_dll_cycle_resource_balance_child PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(libc-osutil-tests libc_dll_cycle_resource_balance_child)
get_fq_target_name(libc_dll_cycle_resource_balance_test
                   libc_dll_cycle_resource_balance_test_target)
add_dependencies(${libc_dll_cycle_resource_balance_test_target}.__build__
                 libc_dll_cycle_resource_balance_child)
target_compile_definitions(
  ${libc_dll_cycle_resource_balance_test_target}.__build__
  PRIVATE
    "LIBC_DLL_CYCLE_RESOURCE_BALANCE_CHILD_PATH=\"$<TARGET_FILE:libc_dll_cycle_resource_balance_child>\""
)
