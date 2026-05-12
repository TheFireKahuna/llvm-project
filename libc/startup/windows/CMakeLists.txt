# Windows startup code for LLVM libc
#
# This provides mainCRTStartup and DllMainCRTStartup entry points for
# Windows executables and DLLs built with LLVM libc.

function(add_startup_object name)
  cmake_parse_arguments(
    "ADD_STARTUP_OBJECT"
    "" # Option argument
    "OUTPUT_NAME;SRC"   # Single value arguments
    "DEPENDS;COMPILE_OPTIONS" # Multi value arguments
    ${ARGN}
  )

  get_fq_target_name(${name} fq_target_name)

  add_object_library(
    ${name}
    SRCS ${ADD_STARTUP_OBJECT_SRC}
    DEPENDS ${ADD_STARTUP_OBJECT_DEPENDS}
    COMPILE_OPTIONS ${ADD_STARTUP_OBJECT_COMPILE_OPTIONS}
  )
  if(ADD_STARTUP_OBJECT_OUTPUT_NAME)
    set(public_output_name ${ADD_STARTUP_OBJECT_OUTPUT_NAME})
  else()
    set(public_output_name ${name}.obj)
  endif()
  set_target_properties(
    ${fq_target_name}
    PROPERTIES
      OUTPUT_NAME ${public_output_name}
  )

  set(staged_output_dir ${LIBC_LIBRARY_DIR})
  set(staged_output ${staged_output_dir}/${public_output_name})
  if((WIN32_ITANIUM OR WIN32_NTPOSIX) AND LLVM_LIBRARY_OUTPUT_INTDIR)
    set(staged_output_dir ${LLVM_LIBRARY_OUTPUT_INTDIR})
    set(staged_output ${staged_output_dir}/${public_output_name})
  endif()
  add_custom_command(
    OUTPUT ${staged_output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${staged_output_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_OBJECTS:${fq_target_name}> ${staged_output}
    DEPENDS ${fq_target_name}
    COMMENT "Staging ${public_output_name} for ambient Windows Itanium startup"
    VERBATIM
  )
  add_custom_target(${name}_ambient DEPENDS ${staged_output})
  set_target_properties(
    ${fq_target_name}
    PROPERTIES
      LIBC_PUBLIC_OUTPUT ${staged_output}
      LIBC_PUBLIC_TARGET ${name}_ambient
  )
endfunction()

if(NOT (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_ARCHITECTURE}))
  message(STATUS "Skipping startup for target architecture ${LIBC_TARGET_ARCHITECTURE}")
  return()
endif()

add_subdirectory(${LIBC_TARGET_ARCHITECTURE})

# Tier A bootstrap — compiled into the static archive and pulled into c.dll
# via WHOLEARCHIVE. Provides __libc_bootstrap(), which brings the process
# into a state where code can execute safely (PCB Zone 0, master VEH,
# identity, Zone 0 seal). Called first from both _DllMainCRTStartup and
# crt_do_start.obj before any Tier B init or C++ ctor runs.
add_object_library(
  libc_bootstrap
  SRCS
    libc_bootstrap.cpp
  DEPENDS
    libc.hdr.stdint_proxy
    libc.src.__support.CPP.atomic
    libc.src.__support.OSUtil.windows.libc_subsystem_init
    libc.src.__support.OSUtil.windows.nt_capabilities
    libc.src.__support.OSUtil.windows.ntdll
    libc.src.__support.OSUtil.windows.process_control_block
    # Tier A now installs the master VEH and sweeps .libcveh before
    # sealing Zone 0 — pull the VEH targets in directly so we don't
    # rely on transitive Tier B inclusion.
    libc.src.__support.OSUtil.windows.veh_reentry_guard
    libc.src.__support.OSUtil.windows.veh_core
    libc.src.__support.OSUtil.windows.veh_filter_registry
    # Memory-primitive walker — runs at Phase 0c.5 and subsumes the
    # former mapping_table_startup_init path. Pulls in the walker
    # implementation + the .libcmem bookends.
    libc.src.__support.OSUtil.windows.memory_primitives_bootstrap
    # Layer 0 PAL — registers a `.libcmem$P0` handler that probes
    # ProcessCookie + SeLockMemoryPrivilege, plus a fork-reinit hook.
    libc.src.__support.OSUtil.windows.nt_pal_init
    # Layer 2 pagemap — registers a `.libcmem$P3` handler that
    # reserves the 16 B / 16 KiB chunkmap (split-half 8 B + 8 B
    # entries) and the two-tier demand-commit metadata (L0 + L1
    # state-machine arrays); registers a fork-reinit hook at
    # priority 37.
    libc.src.__support.OSUtil.windows.pagemap
    # Sealed-VA publisher — bootstrap publishes the pagemap range
    # post-memory-init and runs the seal-time invariant assert + wipe
    # immediately before pcb_seal_readonly_a.
    libc.src.__support.OSUtil.windows.alloc_sealed_va_publisher
    # `memory_lock_policy_state` retired — the per-region MLOCK_ONFAULT
    # bit on the va_tracker desc replaces the global onfault table; the
    # process-wide `mcl_flags` lives in `g_pcb.mlock` (PCB Zone 1).
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Tier B init — compiled into the static archive and pulled into c.dll via
# WHOLEARCHIVE. Provides __libc_init() which crt_do_start.obj calls at EXE
# startup to set up argv/environ/TLS and trigger subsystem bring-up. Runs
# after __libc_bootstrap().
add_object_library(
  libc_init
  SRCS
    libc_init.cpp
  DEPENDS
    libc.config.app_h
    libc.hdr.stdint_proxy
    libc.src.__support.OSUtil.windows.fd_table
    libc.src.__support.OSUtil.windows.libc_subsystem_init
    libc.src.__support.OSUtil.windows.libc_subsystem_fini
    libc.src.__support.threads.thread
    libc.src.__support.OSUtil.windows.signal_subsystem
    libc.src.stdlib.free
    libc.src.stdlib.malloc
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# EXE-side startup — calls __libc_init (from c.dll), walks CRT sections,
# calls main(), exits. No internal libc deps — uses only extern "C" imports.
add_startup_object(
  do_start
  OUTPUT_NAME
    crt_do_start.obj
  SRC
    do_start.cpp
  DEPENDS
    libc.startup.windows.libc_bootstrap
    libc.startup.windows.libc_init
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Generic DLL entry point — still used by dll_crt.cpp (bundled into dllcrt.obj
# for user DLLs). c.dll now uses libc_dll_entry instead.
add_startup_object(
  dll_startup
  SRC
    dll_startup.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# c.dll dedicated entry point — uses explicit __libc_dll_init() instead of
# .CRT$XI* section walking. Only linked into c.dll, not user DLLs.
add_startup_object(
  libc_dll_entry
  SRC
    libc_dll_entry.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Bundled DLL CRT — installed as dllcrt.obj for DLLs that don't have libc
# (libunwind, libc++, user DLLs). Includes self-contained atexit and
# weak __cxa_thread_finalize so dll_startup.cpp's references resolve.
add_startup_object(
  dll_crt
  OUTPUT_NAME
    dllcrt.obj
  SRC
    dll_crt.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# PE infrastructure objects — required for any binary to link.
# These provide linker-required symbols (__security_cookie, _tls_used,
# _load_config_used, __guard_check_icall_fptr) that the PE loader and
# lld-link expect.

add_startup_object(
  security_cookie
  OUTPUT_NAME
    crt_gs.obj
  SRC
    security_cookie.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.bcryptprimitives
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  tls_directory
  OUTPUT_NAME
    crt_tls.obj
  SRC
    tls_directory.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# .CRT$XLC slot that registers __llvm_libc_thread_detach_cleanup. Split
# out of tls_directory.cpp so c.dll can link the rest of the TLS
# infrastructure (_tls_index, IMAGE_TLS_DIRECTORY, .CRT$XLAA cookie init,
# .CRT$XLB dyn TLS init) without also publishing a duplicate cleanup slot.
# This object goes in the consumer EXE only — when LdrShutdownThread walks
# loaded modules on thread exit, only the EXE's slot fires our cleanup,
# guaranteeing exactly-once invocation by linker geometry.
#
# DEPENDS on tls_thread_detach so freestanding-unit test links (which
# aggregate __internal__ objects instead of c.lib) can still resolve
# __llvm_libc_thread_detach_cleanup through the DEPS walker.
add_startup_object(
  tls_cleanup_callback
  OUTPUT_NAME
    crt_tls_cleanup.obj
  SRC
    tls_cleanup_callback.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.tls_thread_detach
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  cfguard
  OUTPUT_NAME
    crt_cfg.obj
  SRC
    cfguard.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  load_config
  OUTPUT_NAME
    crt_loadcfg.obj
  SRC
    load_config.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_startup_object(
  delayload
  OUTPUT_NAME
    crt_delayload.obj
  SRC
    delayload.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.windows_util
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# RTTI section sealing — seals .rdata$ti (dynamically-initialized typeinfo)
# as irrevocably read-only via SEC_NO_CHANGE after init ctors run.
add_startup_object(
  rtti_seal
  OUTPUT_NAME
    crt_rtti_seal.obj
  SRC
    rtti_seal.cpp
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Hermetic EXE startup aggregate. No OUTPUT_NAME / ambient staging — this
# target exists only to expose all hermetic-EXE startup pieces via DEPS for
# `add_libc_hermetic` (which references `libc.startup.<os>.crt1` per upstream
# convention). `get_object_files_for_test` walks DEPS recursively and
# collects every OBJECT_FILE it finds, so the bench link ends up with the
# full mainCRTStartup → __libc_do_start → __libc_init chain plus the PE
# infrastructure objects (security_cookie, tls_directory, cfguard,
# load_config) that lld-link requires in every PE binary.
add_object_library(
  crt1
  SRCS
    crt1_hermetic.cpp
  DEPENDS
    libc.startup.windows.${LIBC_TARGET_ARCHITECTURE}.start
    libc.startup.windows.do_start
    libc.startup.windows.libc_bootstrap
    libc.startup.windows.libc_init
    libc.startup.windows.security_cookie
    libc.startup.windows.tls_directory
    libc.startup.windows.tls_cleanup_callback
    libc.startup.windows.cfguard
    libc.startup.windows.load_config
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

# Universal ABI floor every hermetic Windows exe needs — kept sibling to
# `crt1` so a DllMain-hosted hermetic exe can drop crt1 without losing it.
# Pure DEPS aggregate (empty TU); `add_libc_hermetic`'s Windows branch
# appends this unconditionally so per-bench CMakeLists never re-declare the
# ABI/fault-handler floor.
#
# Only C++-mangled internal OBJ libraries belong here — get_object_files_for_test
# pulls entrypoints as their `__internal__` variant (namespaced mangled names
# only), which cannot satisfy do_start.cpp's extern "C" `exit`/`atexit`/setjmp
# refs. Those are wired with their PUBLIC OBJECT_FILE directly in
# add_libc_hermetic's Windows branch.
add_object_library(
  hermetic_runtime
  SRCS
    hermetic_runtime.cpp
  DEPENDS
    libc.src.__support.macros.config
    libc.src.__support.OSUtil.windows.cxa_guard
    libc.src.__support.OSUtil.windows.mem_fault_handler
    # Entrypoint __internal__ variants provide the namespaced LIBC_NAMESPACE::X
    # symbols called from inside libc. hermetic_runtime.cpp forwards the
    # extern "C" `exit` public alias to LIBC_NAMESPACE::exit; setjmp's
    # extern "C" alias is emitted from setjmp_windows.cpp directly.
    # HermeticTestUtils.cpp provides the atexit shim the same way.
    libc.src.stdlib.atexit
    libc.src.stdlib.exit
    libc.src.setjmp.setjmp
    # libc_init.cpp references LIBC_NAMESPACE::environ (via `using namespace
    # LIBC_NAMESPACE` at top of __libc_init) to wire the POSIX env export.
    libc.src.unistd.environ
  COMPILE_OPTIONS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
)

add_custom_target(libc-startup)
set(startup_components
  start do_start dll_startup libc_dll_entry dll_crt
  security_cookie tls_directory cfguard load_config delayload rtti_seal
)
foreach(target IN LISTS startup_components)
  if(TARGET libc.startup.windows.${target})
    set(fq_target_name libc.startup.windows.${target})
  elseif(TARGET libc.startup.windows.${LIBC_TARGET_ARCHITECTURE}.${target})
    set(fq_target_name libc.startup.windows.${LIBC_TARGET_ARCHITECTURE}.${target})
  else()
    continue()
  endif()
  get_target_property(public_output ${fq_target_name} LIBC_PUBLIC_OUTPUT)
  get_target_property(public_target ${fq_target_name} LIBC_PUBLIC_TARGET)
  add_dependencies(libc-startup ${public_target})
  install(FILES ${public_output}
          DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
          COMPONENT libc)
endforeach()
