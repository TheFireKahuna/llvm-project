if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  basename
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.basename
)

add_entrypoint_object(
  dirname
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.dirname
)
