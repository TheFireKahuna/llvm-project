function(collect_object_file_deps target result)
  # NOTE: This function does add entrypoint targets to |result|.
  # It is expected that the caller adds them separately.

  if(NOT TARGET ${target})
    if(LLVM_LIBC_FULL_BUILD)
      message(FATAL_ERROR "Missing full-build dependency target ${target}")
    endif()
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "")
    set(${result} "" PARENT_SCOPE)
    return()
  endif()

  # Memoization: avoid re-walking the same subtree. Deep dependency graphs
  # (e.g., Windows libc with 39+ OSUtil support targets) cause exponential
  # re-traversal without this cache.
  get_property(_cache_state GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}")
  if(_cache_state STREQUAL "DONE")
    get_property(_cached GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}")
    set(${result} ${_cached} PARENT_SCOPE)
    return()
  elseif(_cache_state STREQUAL "IN_PROGRESS")
    set(${result} "" PARENT_SCOPE)
    return()
  endif()
  set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "IN_PROGRESS")
  set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "")

  set(all_deps "")
  get_target_property(target_type ${target} "TARGET_TYPE")
  if(NOT target_type)
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    return()
  endif()

  if(${target_type} STREQUAL ${OBJECT_LIBRARY_TARGET_TYPE})
    list(APPEND all_deps ${target})
    get_target_property(deps ${target} "DEPS")
    foreach(dep IN LISTS deps)
      collect_object_file_deps(${dep} dep_targets)
      list(APPEND all_deps ${dep_targets})
    endforeach(dep)
    list(REMOVE_DUPLICATES all_deps)
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "${all_deps}")
    set(${result} ${all_deps} PARENT_SCOPE)
    return()
  endif()

  if(${target_type} STREQUAL ${ENTRYPOINT_OBJ_TARGET_TYPE})
    set(entrypoint_target ${target})
    get_target_property(is_alias ${entrypoint_target} "IS_ALIAS")
    if(is_alias)
      get_target_property(aliasee ${entrypoint_target} "DEPS")
      if(NOT aliasee)
        message(FATAL_ERROR
                "Entrypoint alias ${entrypoint_target} does not have an aliasee.")
      endif()
      set(entrypoint_target ${aliasee})
    endif()
    get_target_property(deps ${target} "DEPS")
    foreach(dep IN LISTS deps)
      collect_object_file_deps(${dep} dep_targets)
      list(APPEND all_deps ${dep_targets})
    endforeach(dep)
    list(REMOVE_DUPLICATES all_deps)
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "${all_deps}")
    set(${result} ${all_deps} PARENT_SCOPE)
    return()
  endif()

  if(${target_type} STREQUAL ${ENTRYPOINT_EXT_TARGET_TYPE})
    # It is not possible to recursively extract deps of external dependencies.
    # So, we just accumulate the direct dep and return.
    get_target_property(deps ${target} "DEPS")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "${deps}")
    set(${result} ${deps} PARENT_SCOPE)
    return()
  endif()

  if(${target_type} STREQUAL ${HDR_LIBRARY_TARGET_TYPE})
    # Header libraries produce no objects, but may depend on object libraries
    # (e.g., futex_utils -> wait_slot). Recurse to collect transitive objects.
    get_target_property(deps ${target} "DEPS")
    foreach(dep IN LISTS deps)
      if(TARGET ${dep})
        collect_object_file_deps(${dep} dep_targets)
        list(APPEND all_deps ${dep_targets})
      endif()
    endforeach(dep)
    list(REMOVE_DUPLICATES all_deps)
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
    set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "${all_deps}")
    set(${result} ${all_deps} PARENT_SCOPE)
    return()
  endif()

  set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_STATE_${target}" "DONE")
  set_property(GLOBAL PROPERTY "COLLECT_OBJ_DEPS_CACHE_${target}" "")
endfunction(collect_object_file_deps)

function(get_all_object_file_deps result fq_deps_list)
  set(all_deps "")
  foreach(dep ${fq_deps_list})
    get_target_property(dep_type ${dep} "TARGET_TYPE")
    if(NOT ((${dep_type} STREQUAL ${ENTRYPOINT_OBJ_TARGET_TYPE}) OR
            (${dep_type} STREQUAL ${ENTRYPOINT_EXT_TARGET_TYPE})))
      message(FATAL_ERROR "Dependency '${dep}' of 'add_entrypoint_collection' is "
                          "not an 'add_entrypoint_object' or 'add_entrypoint_external' target.")
    endif()
    collect_object_file_deps(${dep} recursive_deps)
    list(APPEND all_deps ${recursive_deps})
    # Add the entrypoint object target explicitly as collect_object_file_deps
    # only collects object files from non-entrypoint targets.
    if(${dep_type} STREQUAL ${ENTRYPOINT_OBJ_TARGET_TYPE})
      set(entrypoint_target ${dep})
      get_target_property(is_alias ${entrypoint_target} "IS_ALIAS")
      if(is_alias)
        get_target_property(aliasee ${entrypoint_target} "DEPS")
        if(NOT aliasee)
          message(FATAL_ERROR
                  "Entrypoint alias ${entrypoint_target} does not have an aliasee.")
        endif()
        set(entrypoint_target ${aliasee})
      endif()
    endif()
    list(APPEND all_deps ${entrypoint_target})
  endforeach(dep)
  list(REMOVE_DUPLICATES all_deps)
  set(${result} ${all_deps} PARENT_SCOPE)
endfunction()

# A rule to build a library from a collection of entrypoint objects and bundle
# it in a single LLVM-IR bitcode file.
# Usage:
#     add_gpu_entrypoint_library(
#       DEPENDS <list of add_entrypoint_object targets>
#     )
function(add_bitcode_entrypoint_library target_name base_target_name)
  cmake_parse_arguments(
    "ENTRYPOINT_LIBRARY"
    "" # No optional arguments
    "" # No single value arguments
    "DEPENDS" # Multi-value arguments
    ${ARGN}
  )
  if(NOT ENTRYPOINT_LIBRARY_DEPENDS)
    message(FATAL_ERROR "'add_entrypoint_library' target requires a DEPENDS list "
                        "of 'add_entrypoint_object' targets.")
  endif()

  get_fq_deps_list(fq_deps_list ${ENTRYPOINT_LIBRARY_DEPENDS})
  get_all_object_file_deps(all_deps "${fq_deps_list}")

  set(objects "")
  foreach(dep IN LISTS all_deps)
    set(object $<$<STREQUAL:$<TARGET_NAME_IF_EXISTS:${dep}>,${dep}>:$<TARGET_OBJECTS:${dep}>>)
    list(APPEND objects ${object})
  endforeach()

  add_executable(${target_name} ${objects})
  if(LIBC_TARGET_ARCHITECTURE_IS_SPIRV)
      target_link_options(${target_name} PRIVATE "${LIBC_COMPILE_OPTIONS_DEFAULT}"
                      "-nostdlib" "-emit-llvm")
  else()  
      target_link_options(${target_name} PRIVATE "${LIBC_COMPILE_OPTIONS_DEFAULT}"
                      "-r" "-nostdlib" "-flto" "-Wl,--lto-emit-llvm")
  endif()
endfunction(add_bitcode_entrypoint_library)

# A rule to build a library from a collection of entrypoint objects.
# Usage:
#     add_entrypoint_library(
#       DEPENDS <list of add_entrypoint_object targets>
#     )
#
# NOTE: If one wants an entrypoint to be available in a library, then they will
# have to list the entrypoint target explicitly in the DEPENDS list. Implicit
# entrypoint dependencies will not be added to the library.
function(add_entrypoint_library target_name)
  cmake_parse_arguments(
    "ENTRYPOINT_LIBRARY"
    "" # No optional arguments
    "" # No single value arguments
    "DEPENDS" # Multi-value arguments
    ${ARGN}
  )
  if(NOT ENTRYPOINT_LIBRARY_DEPENDS)
    message(FATAL_ERROR "'add_entrypoint_library' target requires a DEPENDS list "
                        "of 'add_entrypoint_object' targets.")
  endif()

  get_fq_deps_list(fq_deps_list ${ENTRYPOINT_LIBRARY_DEPENDS})
  get_all_object_file_deps(all_deps "${fq_deps_list}")

  set(objects "")
  foreach(dep IN LISTS all_deps)
    list(APPEND objects $<$<STREQUAL:$<TARGET_NAME_IF_EXISTS:${dep}>,${dep}>:$<TARGET_OBJECTS:${dep}>>)
  endforeach(dep)

  add_library(
    ${target_name}
    STATIC
    ${objects}
  )
  set_target_properties(${target_name} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${LIBC_LIBRARY_DIR})
endfunction(add_entrypoint_library)

# Generate a PE/COFF .def file listing the public C ABI of c.dll.
#
# Writes <binary_dir>/<target_name>.def whose EXPORTS section contains one
# line per non-alias entrypoint (ENTRYPOINT_NAME property) plus a fixed set
# of non-entrypoint bridge symbols that other runtime DLLs resolve against
# c.dll (errno accessor, EXE-side libc initializer, Itanium thread finalizers,
# VEH fault handler trampoline). Sets ${output_var} to the generated path.
#
# c.def replaces per-symbol __LIBC_DLLEXPORT_ATTR on entrypoints so that
# libc.lib (static) carries no export-table pollution and c.dll's exported
# surface is a single diffable artifact.
function(generate_libc_entrypoints_def output_var target_name)
  cmake_parse_arguments("DEF" "" "" "DEPENDS" ${ARGN})

  set(names "")
  foreach(ep IN LISTS DEF_DEPENDS)
    if(NOT TARGET ${ep})
      continue()
    endif()
    get_target_property(ep_type ${ep} "TARGET_TYPE")
    if(NOT ep_type STREQUAL ${ENTRYPOINT_OBJ_TARGET_TYPE})
      continue()
    endif()
    # Platform-alias entrypoints (e.g. libc.src.unistd.close ->
    # .${LIBC_TARGET_OS}.close) are the canonical names listed in
    # TARGET_LLVMLIBC_ENTRYPOINTS; the aliasee is not. Their ENTRYPOINT_NAME
    # is the public symbol we need to export, so do not filter on IS_ALIAS.
    # REMOVE_DUPLICATES below collapses any same-name collisions.
    get_target_property(ep_name ${ep} "ENTRYPOINT_NAME")
    if(ep_name)
      list(APPEND names ${ep_name})
    endif()
  endforeach()
  list(REMOVE_DUPLICATES names)
  list(SORT names)

  # Bridge symbols exported by c.dll that are not in TARGET_LLVMLIBC_ENTRYPOINTS.
  # Every real cross-boundary symbol is listed here; c.dll never uses
  # __LIBC_DLLEXPORT_ATTR on definitions, so the .def is the sole source of
  # truth for what c.dll exports. That keeps internal OBJs free of -export
  # directives and lets consumer tests pull those OBJs without colliding with
  # c.lib's IAT entries.
  #
  # __llvm_libc_errno: thread-local errno carrier. The errno public header
  # expands `errno` to `(*__llvm_libc_errno())`, so every consumer TU that
  # touches errno references this symbol across the DLL boundary.
  #
  # __cxa_guard_acquire / __cxa_guard_release: Itanium-ABI one-time static-init
  # locks. Emitted by the compiler at every function-local static.
  #
  # __cxa_finalize: called by the Itanium ABI to drain DSO atexit lists on
  # DLL detach. crt1 references it as part of normal teardown.
  #
  # __llvm_libc_thread_detach_cleanup: single-entry dispatcher invoked
  # by crt_tls.obj's `.CRT$XLC` TLS callback on DLL_THREAD_DETACH /
  # DLL_PROCESS_DETACH. Owns the FaultGuard wrap around
  # __cxa_thread_finalize and the libc-internal tls_cleanup walker so
  # crt_tls.obj (linked into every consumer EXE) doesn't take cross-
  # module references on g_pcb / __llvm_libc_setjmp / namespaced libc
  # internals.
  #
  # __libc_bootstrap: Tier A startup — brings the process into a state where
  # code can execute safely (PCB Zone 0, master VEH, identity, Zone 0 seal).
  # crt_do_start.obj calls this before __libc_init.
  #
  # __libc_init: Tier B startup — invoked from crt_do_start.obj (linked into
  # every consumer exe) to perform EXE-specific libc setup (argv, environ,
  # TLS, signals) and trigger subsystem bring-up.
  set(bridge_exports
    __llvm_libc_errno
    # setjmp / sigsetjmp: the public <setjmp.h> macros rewrite
    #   setjmp(buf)           -> __llvm_libc_setjmp((buf), __builtin_frame_address(0))
    #   sigsetjmp(buf, ss)    -> __llvm_libc_sigsetjmp((buf), (ss), __builtin_frame_address(0))
    # so consumer TUs take a cross-DLL reference on these internal names. The
    # entry-point names `setjmp` / `sigsetjmp` are in skip_exports because no
    # symbol by those bare names is emitted under PUBLIC_PACKAGING — the
    # underlying two-arg/three-arg __llvm_libc_* entries carry the real code.
    # (Mirrors __llvm_libc_errno: errno macro -> (*__llvm_libc_errno()).)
    __llvm_libc_setjmp
    __llvm_libc_sigsetjmp
    __cxa_guard_acquire
    __cxa_guard_release
    __cxa_finalize
    __llvm_libc_thread_detach_cleanup
    __libc_bootstrap
    __libc_init
    # veh_core's DLL-unload notification calls this to run the per-image
    # atexit chain before the image unmaps. Consumers link c.dll, so
    # exporting it here lets them resolve the ref without pulling
    # libc.startup.windows.libc_init's full obj (which would duplicate
    # __libc_init/__libc_bootstrap).
    __libc_dll_unload_cxa_finalize

    # GDB/LLDB rendezvous breakpoint site. Cross-process debuggers set a
    # software breakpoint on this function and re-read _r_debug.r_map on
    # each hit. Exporting by name lets the debugger resolve the address
    # from the PE export table without needing _r_debug to be populated
    # first. _dl_debug_state is a same-address alias for older stubs.
    _r_debug_state
    _dl_debug_state)

  # PTY debug hooks — test-only entry points used by pty_probe and the
  # vt_pty_* test binaries to observe slave-side Input handle state without
  # going through the regular POSIX surface. Not part of the stable ABI but
  # still live in c.dll so test executables can dlsym them via c.lib.
  list(APPEND bridge_exports
    __llvm_libc_pty_debug_pending
    __llvm_libc_pty_debug_input_mode
    __llvm_libc_pty_debug_peek_input
    __llvm_libc_pty_debug_single_read
    __llvm_libc_pty_debug_inject_key)

  set(bridge_mangled_exports)

  # DATA exports (non-function bridges). lld-link requires the DATA suffix on
  # /DEF entries that refer to variables so it doesn't generate a function
  # thunk and mismatch the import-library relocation.
  #
  # __stack_chk_guard: stack-protector cookie. Compiler-emitted on every
  # -fstack-protector function prologue; consumer .obj references it
  # indirectly (.refptr.__stack_chk_guard trampoline).
  #
  # _r_debug / _r_debug_pe: GDB/LLDB solib rendezvous structs. PE has no
  # DT_DEBUG tag — cross-process debuggers discover the module list by
  # resolving these symbols from c.dll's export table. _r_debug is the
  # glibc-compatible view (struct r_debug / struct link_map); _r_debug_pe
  # is the PE-aware extension with PDB GUID/age, .pdata VA, etc.
  set(bridge_data_exports
    __stack_chk_guard
    _r_debug
    _r_debug_pe)

  # Entry-point names that have no backing symbol in c.dll because user code
  # accesses them through a header macro (e.g. errno -> (*__llvm_libc_errno()),
  # setjmp -> __llvm_libc_setjmp via the Windows setjmp macro that captures
  # the caller's establisher frame). lld-link treats /DEF entries as implicit
  # root references, so listing these would fail with "undefined symbol".
  set(skip_exports
    errno
    setjmp
    sigsetjmp)

  set(def_file "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.def")
  # No LIBRARY directive — let lld-link pick up the DLL's actual OUTPUT_NAME
  # (c.dll, not libc_shared.dll). A stale LIBRARY would be embedded in the
  # generated import library and break consumers at load time.
  set(content "EXPORTS\n")
  foreach(n IN LISTS names)
    if(n IN_LIST skip_exports)
      continue()
    endif()
    string(APPEND content "  ${n}\n")
  endforeach()
  foreach(n IN LISTS bridge_exports)
    string(APPEND content "  ${n}\n")
  endforeach()
  foreach(n IN LISTS bridge_mangled_exports)
    string(APPEND content "  ${n}\n")
  endforeach()
  foreach(n IN LISTS bridge_data_exports)
    string(APPEND content "  ${n} DATA\n")
  endforeach()
  file(GENERATE OUTPUT ${def_file} CONTENT "${content}")
  set(${output_var} "${def_file}" PARENT_SCOPE)
endfunction()

# A rule to build a shared library from a collection of entrypoint objects.
# Usage:
#     add_entrypoint_library_shared(
#       DEPENDS <list of add_entrypoint_object targets>
#     )
function(add_entrypoint_library_shared target_name)
  cmake_parse_arguments(
    "ENTRYPOINT_LIBRARY"
    "" # No optional arguments
    "" # No single value arguments
    "DEPENDS" # Multi-value arguments
    ${ARGN}
  )
  if(NOT ENTRYPOINT_LIBRARY_DEPENDS)
    message(FATAL_ERROR "'add_entrypoint_library_shared' target requires a DEPENDS list "
                        "of 'add_entrypoint_object' targets.")
  endif()

  get_fq_deps_list(fq_deps_list ${ENTRYPOINT_LIBRARY_DEPENDS})
  get_all_object_file_deps(all_deps "${fq_deps_list}")

  set(objects "")
  foreach(dep IN LISTS all_deps)
    list(APPEND objects $<$<STREQUAL:$<TARGET_NAME_IF_EXISTS:${dep}>,${dep}>:$<TARGET_OBJECTS:${dep}>>)
  endforeach(dep)

  if(WIN32)
    # COFF shared-library links do not scale well when we hand lld-link the
    # full llvm-libc object set directly. Build a single archive first, then
    # whole-archive that into the DLL so we keep the same exported surface with
    # a much smaller link graph.
    set(archive_target "${target_name}.__archive")
    add_library(
      ${archive_target}
      STATIC
      ${objects}
    )
    set_target_properties(${archive_target} PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY ${LIBC_LIBRARY_DIR}
    )

    add_library(${target_name} SHARED)
    if(MSVC OR WIN32_ITANIUM OR WIN32_NTPOSIX OR
       CMAKE_LINKER MATCHES [[(^|[/\\])lld-link(\\.exe)?$]])
      target_link_options(${target_name} PRIVATE
        "LINKER:/WHOLEARCHIVE:$<TARGET_FILE:${archive_target}>"
      )
    else()
      target_link_libraries(${target_name} PRIVATE
        "-Wl,--whole-archive"
        ${archive_target}
        "-Wl,--no-whole-archive"
      )
    endif()
    target_link_libraries(${target_name} PRIVATE ${archive_target})

    # PE/COFF: .def-driven exports. c.dll's ABI surface lives in c.def,
    # generated at configure time from TARGET_LLVMLIBC_ENTRYPOINTS. Without
    # this, the DLL would expose __LIBC_DLLIMPORT_ATTR-tagged symbols
    # in the source (bridge symbols) because the blanket per-entrypoint
    # dllexport was removed in favour of this single diffable artifact.
    generate_libc_entrypoints_def(
      _libc_def_file ${target_name}
      DEPENDS ${fq_deps_list})
    target_link_options(${target_name} PRIVATE
      "LINKER:/DEF:${_libc_def_file}")
  else()
    add_library(
      ${target_name}
      SHARED
      ${objects}
    )
  endif()

  set_target_properties(${target_name} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${LIBC_LIBRARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${LIBC_LIBRARY_DIR}
  )
  if(WIN32)
    # DLLs go to RUNTIME_OUTPUT_DIRECTORY; place them alongside other tools
    # rather than in the lib/ directory.
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${LLVM_RUNTIME_OUTPUT_INTDIR}
    )
  endif()
endfunction(add_entrypoint_library_shared)

set(HDR_LIBRARY_TARGET_TYPE "HDR_LIBRARY")

# Internal function, used by `add_header_library`.
function(create_header_library fq_target_name)
  cmake_parse_arguments(
    "ADD_HEADER"
    "" # Optional arguments
    "" # Single value arguments
    "HDRS;DEPENDS;FLAGS;COMPILE_OPTIONS" # Multi-value arguments
    ${ARGN}
  )

  if(NOT ADD_HEADER_HDRS)
    message(FATAL_ERROR "'add_header_library' target requires a HDRS list of .h files.")
  endif()

  if(SHOW_INTERMEDIATE_OBJECTS)
    message(STATUS "Adding header library ${fq_target_name}")
    if(${SHOW_INTERMEDIATE_OBJECTS} STREQUAL "DEPS")
      foreach(dep IN LISTS ADD_HEADER_DEPENDS)
        message(STATUS "  ${fq_target_name} depends on ${dep}")
      endforeach()
    endif()
  endif()

  add_library(${fq_target_name} INTERFACE)
  target_sources(${fq_target_name} INTERFACE ${ADD_HEADER_HDRS})
  if(ADD_HEADER_DEPENDS)
    add_dependencies(${fq_target_name} ${ADD_HEADER_DEPENDS})

    # `*.__copied_hdr__` is created only to copy the header files to the target
    # location, not to be linked against.
    set(link_lib "")
    foreach(dep ${ADD_HEADER_DEPENDS})
      if (NOT dep MATCHES "__copied_hdr__")
        list(APPEND link_lib ${dep})
      endif()
    endforeach()

    target_link_libraries(${fq_target_name} INTERFACE ${link_lib})
  endif()
  if(ADD_HEADER_COMPILE_OPTIONS)
    target_compile_options(${fq_target_name} INTERFACE ${ADD_HEADER_COMPILE_OPTIONS})
  endif()
  set_target_properties(
    ${fq_target_name}
    PROPERTIES
      INTERFACE_FLAGS "${ADD_HEADER_FLAGS}"
      TARGET_TYPE "${HDR_LIBRARY_TARGET_TYPE}"
      DEPS "${ADD_HEADER_DEPENDS}"
      FLAGS "${ADD_HEADER_FLAGS}"
  )
endfunction(create_header_library)

# Rule to add header only libraries.
# Usage
#    add_header_library(
#      <target name>
#      HDRS  <list of .h files part of the library>
#      DEPENDS <list of dependencies>
#      FLAGS <list of flags>
#    )

function(add_header_library target_name)
  add_target_with_flags(
    ${target_name}
    CREATE_TARGET create_header_library
    ${ARGN}
  )
endfunction(add_header_library)
