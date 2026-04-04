# Windows implementations of backtrace functions via NT primitives.
#
# backtrace            -> RtlCaptureStackBackTrace (single NT call)
# backtrace_symbols    -> RtlPcToFileHeader + PE export table walk
# backtrace_symbols_fd -> same, async-signal-safe write via NtWriteFile

add_entrypoint_object(
  backtrace
  SRCS
    backtrace.cpp
  HDRS
    ../backtrace.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.backtrace_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  backtrace_symbols
  SRCS
    backtrace_symbols.cpp
  HDRS
    ../backtrace_symbols.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.backtrace_ops
    libc.src.__support.OSUtil.windows.ntdll
)

add_entrypoint_object(
  backtrace_symbols_fd
  SRCS
    backtrace_symbols_fd.cpp
  HDRS
    ../backtrace_symbols_fd.h
  DEPENDS
    libc.src.__support.common
    libc.src.__support.OSUtil.windows.backtrace_ops
    libc.src.__support.OSUtil.windows.ntdll
)
