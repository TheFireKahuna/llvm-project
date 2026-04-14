# Interactive ConDrv terminal demo — exercises POSIX terminal APIs (tcgetattr,
# ioctl, signal, read/write) against a live ConPTY host. Not a test; use for
# manual validation of the console subsystem.
#
# Build: ninja condrv_terminal_demo
# Run:   condrv_terminal_demo.cmd (launches in a new ConPTY session)

_get_common_test_compile_options(condrv_terminal_demo_compile_options "" "")
get_link_options(condrv_terminal_demo_link_options
                 ${condrv_terminal_demo_compile_options})

add_executable(
  condrv_terminal_demo
  EXCLUDE_FROM_ALL
  condrv_terminal_demo.cpp
)
configure_file(
  condrv_terminal_demo.cmd.in
  ${CMAKE_CURRENT_BINARY_DIR}/condrv_terminal_demo.cmd
  COPYONLY
)
target_include_directories(condrv_terminal_demo SYSTEM PRIVATE
                           ${LIBC_SOURCE_DIR}/include)
target_include_directories(condrv_terminal_demo SYSTEM PRIVATE
                           ${LIBC_INCLUDE_DIR})
target_include_directories(condrv_terminal_demo PRIVATE ${LIBC_SOURCE_DIR})
target_include_directories(condrv_terminal_demo PRIVATE ${LIBC_BUILD_DIR})
target_compile_options(condrv_terminal_demo PRIVATE
                       ${condrv_terminal_demo_compile_options})
target_link_directories(condrv_terminal_demo PRIVATE ${LLVM_BINARY_DIR}/lib)
target_link_options(condrv_terminal_demo PRIVATE
                    ${condrv_terminal_demo_link_options})
set_target_properties(
  condrv_terminal_demo
  PROPERTIES
    CXX_STANDARD ${CMAKE_CXX_STANDARD}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
add_custom_command(
  TARGET condrv_terminal_demo
  POST_BUILD
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:libc_shared>
    $<TARGET_FILE_DIR:condrv_terminal_demo>/c.dll
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_BINARY_DIR}/condrv_terminal_demo.cmd
    $<TARGET_FILE_DIR:condrv_terminal_demo>/condrv_terminal_demo.cmd
  VERBATIM
)
target_link_libraries(
  condrv_terminal_demo
  PRIVATE windows_ntdll
          windows_sspicli
          ${LLVM_BINARY_DIR}/lib/c.lib)
add_dependencies(
  condrv_terminal_demo
  libc_shared
  libc-startup
)
