if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_setjmp_src setjmp_windows.cpp)
else()
  set(_setjmp_src setjmp.cpp)
endif()

add_entrypoint_object(
  setjmp
  SRCS
    ${_setjmp_src}
  HDRS
    ../setjmp_impl.h
  DEPENDS
    libc.hdr.offsetof_macros
    libc.hdr.types.jmp_buf
)

if(LIBC_TARGET_OS_IS_WINDOWS)
  set(_longjmp_src longjmp_windows.cpp)
  set(_longjmp_deps
    libc.hdr.types.jmp_buf
    libc.src.__support.OSUtil.osutil
    libc.src.__support.OSUtil.windows.signal_types_h
  )
else()
  set(_longjmp_src longjmp.cpp)
  set(_longjmp_deps libc.hdr.types.jmp_buf)
endif()

add_entrypoint_object(
  longjmp
  SRCS
    ${_longjmp_src}
  HDRS
    ../longjmp.h
  DEPENDS
    ${_longjmp_deps}
)

if (TARGET libc.src.setjmp.sigsetjmp_epilogue)
  if(LIBC_TARGET_OS_IS_WINDOWS)
    set(_sigsetjmp_src sigsetjmp_windows.cpp)
    set(_sigsetjmp_deps
      libc.hdr.types.sigjmp_buf
      libc.hdr.types.sigset_t
      libc.hdr.offsetof_macros
      libc.hdr.signal_macros
      libc.src.setjmp.setjmp
      libc.src.signal.sigprocmask
    )
  else()
    set(_sigsetjmp_src sigsetjmp.cpp)
    set(_sigsetjmp_deps
      libc.hdr.types.sigjmp_buf
      libc.hdr.types.sigset_t
      libc.hdr.offsetof_macros
      libc.src.setjmp.sigsetjmp_epilogue
      libc.src.setjmp.setjmp
    )
  endif()

  add_entrypoint_object(
    sigsetjmp
    SRCS
      ${_sigsetjmp_src}
    HDRS
      ../sigsetjmp.h
    DEPENDS
      ${_sigsetjmp_deps}
  )
endif()
