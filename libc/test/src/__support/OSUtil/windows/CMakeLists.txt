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

# publish_handles rollback regression test — internal API, no c.dll needed.
add_libc_test(
  publish_handles_rollback_test
  SUITE
    libc-osutil-tests
  SRCS
    publish_handles_rollback_test.cpp
  DEPENDS
    libc.src.__support.OSUtil.windows.process_console
    libc.src.__support.OSUtil.windows.ipc_condrv
  LINK_LIBRARIES
    windows_ntdll
)

# ===----------------------------------------------------------------------===
# PTY and terminal tests — exercise POSIX terminal APIs end-to-end.
# These link against the full shared libc (c.dll) and require a ConPTY host.
# ===----------------------------------------------------------------------===

# Helper macro to build a test executable against c.dll with the LLVM libc
# test framework. Used for tests that exercise the full POSIX API surface.
macro(add_cdll_test target_name)
  cmake_parse_arguments(ARG "" "" "SRCS;EXTRA_DEPS" ${ARGN})

  _get_common_test_compile_options(${target_name}_compile_options "" "")
  get_link_options(${target_name}_link_options ${${target_name}_compile_options})

  add_executable(
    ${target_name}
    EXCLUDE_FROM_ALL
    ${ARG_SRCS}
  )
  target_include_directories(${target_name} SYSTEM PRIVATE
                             ${LIBC_SOURCE_DIR}/include)
  target_include_directories(${target_name} SYSTEM PRIVATE
                             ${LIBC_INCLUDE_DIR})
  target_include_directories(${target_name} PRIVATE ${LIBC_SOURCE_DIR})
  target_include_directories(${target_name} PRIVATE ${LIBC_BUILD_DIR})
  target_compile_options(${target_name} PRIVATE
                         ${${target_name}_compile_options})
  target_link_directories(${target_name} PRIVATE ${LLVM_BINARY_DIR}/lib)
  target_link_options(${target_name} PRIVATE
                      ${${target_name}_link_options})
  set_target_properties(
    ${target_name}
    PROPERTIES
      CXX_STANDARD ${CMAKE_CXX_STANDARD}
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:libc_shared>
      $<TARGET_FILE_DIR:${target_name}>/c.dll
    VERBATIM
  )
  target_link_libraries(${target_name} PRIVATE windows_ntdll
                                               windows_sspicli
                                               ${LLVM_BINARY_DIR}/lib/c.lib
                                               LibcDeathTestExecutors.unit
                                               LibcTest.unit)
  add_dependencies(
    ${target_name}
    libc_shared
    libc-startup
    ${ARG_EXTRA_DEPS}
  )
  add_dependencies(libc-osutil-tests ${target_name})
  if(TARGET libc-unit-tests-build)
    add_dependencies(libc-unit-tests-build ${target_name})
  endif()
endmacro()

# Helper macro to build a standalone child helper exe (no test framework).
macro(add_cdll_child_helper target_name)
  cmake_parse_arguments(ARG "" "" "SRCS" ${ARGN})

  _get_common_test_compile_options(${target_name}_compile_options "" "")
  get_link_options(${target_name}_link_options ${${target_name}_compile_options})

  add_executable(
    ${target_name}
    EXCLUDE_FROM_ALL
    ${ARG_SRCS}
  )
  target_include_directories(${target_name} SYSTEM PRIVATE
                             ${LIBC_SOURCE_DIR}/include)
  target_include_directories(${target_name} SYSTEM PRIVATE
                             ${LIBC_INCLUDE_DIR})
  target_include_directories(${target_name} PRIVATE ${LIBC_SOURCE_DIR})
  target_include_directories(${target_name} PRIVATE ${LIBC_BUILD_DIR})
  target_compile_options(${target_name} PRIVATE
                         ${${target_name}_compile_options})
  target_link_directories(${target_name} PRIVATE ${LLVM_BINARY_DIR}/lib)
  target_link_options(${target_name} PRIVATE
                      ${${target_name}_link_options})
  set_target_properties(
    ${target_name}
    PROPERTIES
      CXX_STANDARD ${CMAKE_CXX_STANDARD}
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:libc_shared>
      $<TARGET_FILE_DIR:${target_name}>/c.dll
    VERBATIM
  )
  target_link_libraries(${target_name} PRIVATE windows_ntdll
                                               windows_sspicli
                                               ${LLVM_BINARY_DIR}/lib/c.lib)
  add_dependencies(
    ${target_name}
    libc_shared
    libc-startup
  )
endmacro()

# --- Legacy smoke tests (converted to LLVM libc test framework) ---

add_cdll_test(vt_pty_devpts_test
  SRCS vt_pty_devpts_test.cpp
)

# Tree join: child helper + parent test.
add_cdll_child_helper(vt_pty_tree_join_child
  SRCS vt_pty_tree_join_child.cpp
)

add_cdll_test(vt_pty_tree_join_test
  SRCS vt_pty_tree_join_test.cpp
  EXTRA_DEPS vt_pty_tree_join_child
)
target_compile_definitions(vt_pty_tree_join_test PRIVATE
  VT_PTY_TREE_JOIN_CHILD_PATH="$<TARGET_FILE:vt_pty_tree_join_child>"
)

# --- Console TTY behavior tests ---

add_cdll_test(console_tty_test
  SRCS console_tty_test.cpp
)

# --- PTY lifecycle integration tests ---

add_cdll_child_helper(pty_lifecycle_child
  SRCS pty_lifecycle_child.cpp
)

add_cdll_test(pty_lifecycle_test
  SRCS pty_lifecycle_test.cpp
  EXTRA_DEPS pty_lifecycle_child
)
target_compile_definitions(pty_lifecycle_test PRIVATE
  PTY_LIFECYCLE_CHILD_PATH="$<TARGET_FILE:pty_lifecycle_child>"
)

# --- Legacy smoke tests (standalone main, no test framework) ---
# Kept for backwards compatibility; prefer the _test variants above.

add_cdll_child_helper(vt_pty_devpts_smoke
  SRCS vt_pty_devpts_smoke.cpp
)
add_dependencies(libc-osutil-tests vt_pty_devpts_smoke)

add_cdll_child_helper(vt_pty_tree_join_smoke
  SRCS vt_pty_tree_join_smoke.cpp
)
add_dependencies(libc-osutil-tests vt_pty_tree_join_smoke)
