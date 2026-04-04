set(libc_archive_targets "")
set(libc_archive_names "")
set(libc_archive_entrypoint_lists "")
if(LLVM_LIBC_FULL_BUILD)
  list(APPEND libc_archive_names c m mvec)
  list(APPEND libc_archive_targets libc libm libmvec)
  list(APPEND libc_archive_entrypoint_lists
       TARGET_LIBC_ENTRYPOINTS TARGET_LIBM_ENTRYPOINTS TARGET_LIBMVEC_ENTRYPOINTS)
else()
  list(APPEND libc_archive_names llvmlibc)
  list(APPEND libc_archive_targets libc)
  list(APPEND libc_archive_entrypoint_lists TARGET_LLVMLIBC_ENTRYPOINTS)
endif()

set(added_archive_targets "")
# Collect all entrypoints across archives for the unified shared library.
set(all_shared_entrypoints "")

foreach(archive IN ZIP_LISTS
        libc_archive_names libc_archive_targets libc_archive_entrypoint_lists)
  if(NOT ${archive_2})
    # If an entrypoint list is missing, then skip adding targets for it.
    continue()
  endif()

  set(archive_entrypoints "")
  foreach(entrypoint_target IN LISTS ${archive_2})
    if(TARGET ${entrypoint_target})
      list(APPEND archive_entrypoints ${entrypoint_target})
    elseif(NOT LLVM_LIBC_FULL_BUILD)
      if(LIBC_CMAKE_VERBOSE_LOGGING)
        message(STATUS
                "Skipping unavailable non-fullbuild entrypoint target ${entrypoint_target} from archive ${archive_1}")
      endif()
    else()
      message(FATAL_ERROR
              "Missing full-build entrypoint target ${entrypoint_target} for archive ${archive_1}")
    endif()
  endforeach()

  if(NOT archive_entrypoints)
    continue()
  endif()

  #===----------------------------------------------------------------------===#
  # Static library (separate archives: c.lib, m.lib, mvec.lib)
  #===----------------------------------------------------------------------===#
  if(LIBC_ENABLE_STATIC)
    add_entrypoint_library(
      ${archive_1}
      DEPENDS
        ${archive_entrypoints}
    )
    set_target_properties(
      ${archive_1}
      PROPERTIES
        ARCHIVE_OUTPUT_NAME ${archive_0}
    )
    if(LLVM_LIBC_FULL_BUILD)
      target_link_libraries(${archive_1} PUBLIC libc-headers)
      if(TARGET libc-startup)
        add_dependencies(${archive_1} libc-startup)
      endif()
    endif()
    list(APPEND added_archive_targets ${archive_1})
  endif()

  # Accumulate entrypoints for the unified shared library.
  if(LIBC_ENABLE_SHARED)
    list(APPEND all_shared_entrypoints ${archive_entrypoints})
  endif()

  # Add the offloading version of the library for offloading languages. These
  # are installed in the standard search path separate from the other libraries.
  if(LIBC_TARGET_OS_IS_GPU)
    add_bitcode_entrypoint_library(
      ${archive_1}bitcode
      ${archive_1}
      DEPENDS
        ${${archive_2}}
    )
    set_target_properties(
      ${archive_1}bitcode
      PROPERTIES
        OUTPUT_NAME ${archive_1}.bc
    )
    list(APPEND added_bitcode_targets ${archive_1}bitcode)
  endif()
endforeach()

#===------------------------------------------------------------------------===#
# Shared library: single unified DLL (c.dll) containing all entrypoints
#===------------------------------------------------------------------------===#
# Math and vector entrypoints share internal state (errno, allocator, etc.)
# with libc, so they must live in the same shared library. Static archives
# remain separate (c.lib, m.lib, mvec.lib) for traditional link granularity.
set(added_shared_targets "")
if(LIBC_ENABLE_SHARED AND all_shared_entrypoints)
  add_entrypoint_library_shared(
    libc_shared
    DEPENDS
      ${all_shared_entrypoints}
  )
  set_target_properties(
    libc_shared
    PROPERTIES
      OUTPUT_NAME c
      LINKER_LANGUAGE C
  )
  if(LLVM_LIBC_FULL_BUILD)
    target_link_libraries(libc_shared PUBLIC libc-headers)
  endif()

  # Windows: DLLs go to bin/ staging directory when available.
  if(MSVC_LIKE AND LLVM_RUNTIME_OUTPUT_INTDIR)
    set_target_properties(libc_shared PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${LLVM_RUNTIME_OUTPUT_INTDIR})
  endif()

  list(APPEND added_shared_targets libc_shared)
endif()

#===------------------------------------------------------------------------===#
# Windows shared library: link startup objects and kernel import libraries
#===------------------------------------------------------------------------===#
if(added_shared_targets AND LIBC_TARGET_OS_IS_WINDOWS)
  # Startup objects for DLL entry and libc initialization.
  #
  # `tls_directory` provides c.dll's own TLS infrastructure (_tls_index,
  # IMAGE_TLS_DIRECTORY, .CRT$XLAA cookie init, .CRT$XLB dyn TLS init) — c.dll
  # needs all of it because c.dll uses thread_local internally.
  #
  # `tls_cleanup_callback` (the .CRT$XLC slot that registers
  # __llvm_libc_thread_detach_cleanup) is deliberately split out and NOT
  # linked into c.dll. That registration belongs in the consumer EXE only
  # (pulled via crt1's DEPS chain). Linking it into c.dll too would publish
  # a second .CRT$XLC slot, and LdrShutdownThread would then fire cleanup
  # twice per external thread exit — once from c.dll's TLS callbacks and
  # once from the EXE's. Single-firing is structurally enforced by linker
  # geometry, not a runtime guard.
  set(_libc_dll_startup_objects "")
  foreach(_obj libc_dll_entry libc_bootstrap libc_init security_cookie tls_directory cfguard load_config)
    if(TARGET libc.startup.windows.${_obj})
      list(APPEND _libc_dll_startup_objects libc.startup.windows.${_obj})
    endif()
  endforeach()

  # Kernel import libraries generated from .def files.
  set(_libc_import_libs "")
  foreach(_imp ntdll bcryptprimitives sspicli)
    if(TARGET windows_${_imp})
      list(APPEND _libc_import_libs windows_${_imp})
    endif()
  endforeach()

  # These support objects are runtime-owned pieces of c.dll that the
  # ordinary DEPENDS chain doesn't reach (no symbol reference in c.dll's
  # link graph): cxa_guard is reached only via the C++ ABI mangled
  # __cxa_guard_acquire / __cxa_guard_release names, the *_bootstrap
  # walker has its caller on the EXE side, libc_subsystem_init/fini are
  # called from libc_dll_entry via plain extern "C" decls without a
  # CMake DEPENDS edge, and tls_thread_detach is reached only through a
  # .CRT$XLC TLS callback slot.
  #
  # mem_fault_handler used to be listed here too, but is now reached
  # via the LIBC_FORCE_PULL_GLOB("__libc_veh_anchor_*") directive in
  # veh_filter_registry.cpp (itself in c.dll via libc_subsystem_init's
  # DEPENDS chain) — the glob matches the __libc_veh_anchor_mem_fault
  # extern that LIBC_REGISTER_VEH_FILTER emits.
  #
  # libc_fork_reinit_impl + fork_quiesce are pulled transitively via
  # libc.src.__support.OSUtil.windows.fork_reinit's DEPENDS chain plus
  # the LIBC_FORCE_PULL_GLOB("__libc_anchor_fork_*") in fork_reinit.cpp.
  # Listing them here too duplicates the symbols at c.dll link time.
  set(_libc_shared_support_objects "")
  foreach(_obj
          libc.src.__support.OSUtil.windows.cxa_guard
          libc.src.__support.OSUtil.windows.memory_primitives_bootstrap
          libc.src.__support.OSUtil.windows.libc_subsystem_init
          libc.src.__support.OSUtil.windows.libc_subsystem_fini
          libc.src.__support.OSUtil.windows.tls_thread_detach)
    if(TARGET ${_obj})
      list(APPEND _libc_shared_support_objects $<TARGET_OBJECTS:${_obj}>)
    endif()
  endforeach()

  foreach(_shared_target IN LISTS added_shared_targets)
    if(_libc_shared_support_objects)
      target_sources(${_shared_target} PRIVATE ${_libc_shared_support_objects})
    endif()
    if(_libc_dll_startup_objects)
      target_link_libraries(${_shared_target} PRIVATE ${_libc_dll_startup_objects})
    endif()
    if(_libc_import_libs)
      target_link_libraries(${_shared_target} PRIVATE ${_libc_import_libs})
    endif()
    if(TARGET libc-windows-import-libs)
      # Stage NT import libs into the ambient lib directory so the freshly
      # built clang can find them via Dir/../lib during bootstrap.
      add_dependencies(${_shared_target} libc-windows-import-libs)
    endif()
    # -nostartfiles: prevent the driver from adding staged startup objects
    # (dllcrt.obj, crt_gs.obj, etc.) that conflict with the ones we link
    # explicitly above. c.dll provides its own entry point and startup.
    # --unwindlib=none: prevent the driver from adding unwind.lib (not built yet).
    target_link_options(${_shared_target} PRIVATE -nostartfiles --unwindlib=none)
  endforeach()
endif()

#===------------------------------------------------------------------------===#
# Install
#===------------------------------------------------------------------------===#
if(added_archive_targets)
  install(
    TARGETS ${added_archive_targets}
    ARCHIVE DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
    COMPONENT libc
  )
endif()

if(added_shared_targets)
  install(
    TARGETS ${added_shared_targets}
    ARCHIVE DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
    LIBRARY DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
    RUNTIME DESTINATION ${LIBC_INSTALL_RUNTIME_DIR}
    COMPONENT libc
  )
endif()

foreach(file ${added_bitcode_targets})
  install(FILES $<TARGET_FILE:${file}>
          DESTINATION ${LIBC_INSTALL_LIBRARY_DIR}
          RENAME $<TARGET_PROPERTY:${file},OUTPUT_NAME>
          COMPONENT libc
  )
endforeach()

if(NOT LIBC_TARGET_OS_IS_BAREMETAL)
  # For now we will disable libc-startup installation for baremetal. The
  # correct way to do it would be to make a hookable startup for baremetal
  # and install it as part of the libc installation.
  set(startup_target "libc-startup")
endif()

if(LLVM_LIBC_FULL_BUILD)
  set(header_install_target install-libc-headers)
endif()

set(_libc_install_extra_deps "")
if(TARGET libc-windows-import-libs)
  list(APPEND _libc_install_extra_deps libc-windows-import-libs)
endif()

add_custom_target(install-libc
                  DEPENDS ${added_archive_targets}
                          ${added_shared_targets}
                          ${added_bitcode_targets}
                          ${_libc_install_extra_deps}
                          ${startup_target}
                          ${header_install_target}
                  COMMAND "${CMAKE_COMMAND}"
                          -DCMAKE_INSTALL_COMPONENT=libc
                          -P "${CMAKE_BINARY_DIR}/cmake_install.cmake")
add_custom_target(install-libc-stripped
                  DEPENDS ${added_archive_targets}
                          ${added_shared_targets}
                          ${added_bitcode_targets}
                          ${_libc_install_extra_deps}
                          ${startup_target}
                          ${header_install_target}
                  COMMAND "${CMAKE_COMMAND}"
                          -DCMAKE_INSTALL_COMPONENT=libc
                          -DCMAKE_INSTALL_DO_STRIP=1
                          -P "${CMAKE_BINARY_DIR}/cmake_install.cmake")
