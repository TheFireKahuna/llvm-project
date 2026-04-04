if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_entrypoint_object(
  getpwuid
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getpwuid
)

add_entrypoint_object(
  getpwnam
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getpwnam
)

add_entrypoint_object(
  getpwuid_r
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getpwuid_r
)

add_entrypoint_object(
  getpwnam_r
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.getpwnam_r
)
