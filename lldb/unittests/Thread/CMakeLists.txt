if (LLVM_RUNTIME_WIN32)
  list(APPEND LLDB_WINDOWS_LIBS lldbPluginPlatformWindows)
  list(APPEND LLDB_WINDOWS_LIBS lldbPluginProcessWindowsCommon)
endif()

add_lldb_unittest(ThreadTests
  ThreadTest.cpp

  LINK_LIBS
      lldbCore
      lldbHost
      lldbTarget
      lldbSymbol
      lldbUtility
      lldbUtilityHelpers
      lldbInterpreter
      lldbBreakpoint
      lldbPluginPlatformLinux
      ${LLDB_WINDOWS_LIBS}
  )
