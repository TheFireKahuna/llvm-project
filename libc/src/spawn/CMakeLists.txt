if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})
endif()

add_header_library(
  file_actions
  HDRS
    file_actions.h
  DEPENDS
    libc.hdr.stdint_proxy
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawn_file_actions_init
  SRCS
    posix_spawn_file_actions_init.cpp
  HDRS
    posix_spawn_file_actions_init.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawn_file_actions_destroy
  SRCS
    posix_spawn_file_actions_destroy.cpp
  HDRS
    posix_spawn_file_actions_destroy.h
  DEPENDS
    .file_actions
    libc.include.spawn
    libc.src.__support.CPP.new
    libc.src.errno.errno
)

add_entrypoint_object(
  posix_spawn_file_actions_adddup2
  SRCS
    posix_spawn_file_actions_adddup2.cpp
  HDRS
    posix_spawn_file_actions_adddup2.h
  DEPENDS
    .file_actions
    libc.include.spawn
    libc.src.__support.CPP.new
    libc.src.errno.errno
)

add_entrypoint_object(
  posix_spawn_file_actions_addopen
  SRCS
    posix_spawn_file_actions_addopen.cpp
  HDRS
    posix_spawn_file_actions_addopen.h
  DEPENDS
    .file_actions
    libc.include.spawn
    libc.src.__support.CPP.new
    libc.src.errno.errno
)

add_entrypoint_object(
  posix_spawn_file_actions_addclose
  SRCS
    posix_spawn_file_actions_addclose.cpp
  HDRS
    posix_spawn_file_actions_addclose.h
  DEPENDS
    .file_actions
    libc.include.spawn
    libc.src.__support.CPP.new
    libc.src.errno.errno
)

add_entrypoint_object(
  posix_spawnattr_init
  SRCS
    posix_spawnattr_init.cpp
  HDRS
    posix_spawnattr_init.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_destroy
  SRCS
    posix_spawnattr_destroy.cpp
  HDRS
    posix_spawnattr_destroy.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getflags
  SRCS
    posix_spawnattr_getflags.cpp
  HDRS
    posix_spawnattr_getflags.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setflags
  SRCS
    posix_spawnattr_setflags.cpp
  HDRS
    posix_spawnattr_setflags.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getsigdefault
  SRCS
    posix_spawnattr_getsigdefault.cpp
  HDRS
    posix_spawnattr_getsigdefault.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setsigdefault
  SRCS
    posix_spawnattr_setsigdefault.cpp
  HDRS
    posix_spawnattr_setsigdefault.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getsigmask
  SRCS
    posix_spawnattr_getsigmask.cpp
  HDRS
    posix_spawnattr_getsigmask.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setsigmask
  SRCS
    posix_spawnattr_setsigmask.cpp
  HDRS
    posix_spawnattr_setsigmask.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getpgroup
  SRCS
    posix_spawnattr_getpgroup.cpp
  HDRS
    posix_spawnattr_getpgroup.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setpgroup
  SRCS
    posix_spawnattr_setpgroup.cpp
  HDRS
    posix_spawnattr_setpgroup.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getschedparam
  SRCS
    posix_spawnattr_getschedparam.cpp
  HDRS
    posix_spawnattr_getschedparam.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setschedparam
  SRCS
    posix_spawnattr_setschedparam.cpp
  HDRS
    posix_spawnattr_setschedparam.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_getschedpolicy
  SRCS
    posix_spawnattr_getschedpolicy.cpp
  HDRS
    posix_spawnattr_getschedpolicy.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawnattr_setschedpolicy
  SRCS
    posix_spawnattr_setschedpolicy.cpp
  HDRS
    posix_spawnattr_setschedpolicy.h
  DEPENDS
    libc.include.spawn
)

add_entrypoint_object(
  posix_spawn
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.posix_spawn
)

add_entrypoint_object(
  posix_spawnp
  ALIAS
  DEPENDS
    .${LIBC_TARGET_OS}.posix_spawnp
)
