if(LIBC_CONF_SCANF_DISABLE_FLOAT)
  libc_add_definition(scanf_config_copts "LIBC_COPT_SCANF_DISABLE_FLOAT")
endif()
if(LIBC_CONF_SCANF_DISABLE_INDEX_MODE)
  libc_add_definition(scanf_config_copts "LIBC_COPT_SCANF_DISABLE_INDEX_MODE")
endif()
if(scanf_config_copts)
  list(PREPEND scanf_config_copts "COMPILE_OPTIONS")
endif()


list(APPEND file_deps libc.hdr.types.FILE)
if(LIBC_TARGET_OS_IS_GPU)
  list(APPEND file_deps
    libc.src.stdio.getc
    libc.src.stdio.ungetc
    libc.src.stdio.ferror
  )
elseif(LLVM_LIBC_FULL_BUILD)
  list(APPEND file_deps
    libc.src.__support.File.file
  )
endif()

add_header_library(
  scanf_config
  HDRS
    scanf_config.h
  ${scanf_config_copts}
)

add_header_library(
  core_structs
  HDRS
    core_structs.h
  DEPENDS
    .scanf_config
    libc.hdr.types.wchar_t
    libc.include.inttypes
    libc.src.__support.CPP.bitset
    libc.src.__support.FPUtil.fp_bits
)

add_header_library(
  parser
  HDRS
    parser.h
  DEPENDS
    .core_structs
    libc.hdr.types.wchar_t
    libc.src.__support.arg_list
    libc.src.__support.ctype_utils
    libc.src.__support.wctype_utils
    libc.src.__support.str_to_integer
    libc.src.__support.CPP.bit
    libc.src.__support.CPP.bitset
    libc.src.__support.CPP.string_view
    libc.src.__support.CPP.type_traits
)

add_header_library(
  scanf_main
  HDRS
    scanf_main.h
  DEPENDS
    .parser
    .reader
    .converter
    .core_structs
    libc.src.__support.arg_list
  ${use_system_file}
)

add_header_library(
  reader
  HDRS
    reader.h
  DEPENDS
    libc.src.__support.macros.attributes
)

add_header_library(
  string_reader
  HDRS
    string_reader.h
  DEPENDS
    .reader
    libc.src.__support.macros.attributes
)

add_header_library(
  converter
  HDRS
    converter.h
    converter_utils.h
    int_converter.h
    string_converter.h
    float_converter.h
    current_pos_converter.h
    ptr_converter.h
  DEPENDS
    .reader
    .core_structs
    libc.hdr.types.wchar_t
    libc.include.inttypes
    libc.src.__support.common
    libc.src.__support.ctype_utils
    libc.src.__support.wctype_utils
    libc.src.__support.wchar.character_converter
    libc.src.__support.wchar.mbstate
    libc.src.__support.CPP.bitset
    libc.src.__support.CPP.limits
    libc.src.__support.CPP.type_traits
    libc.src.__support.char_vector
    libc.src.__support.str_to_float
  ${use_system_file}
)

#TODO: condense the file-related code as possible.
add_header_library(
    vfscanf_internal
    HDRS
      vfscanf_internal.h
    DEPENDS
      .reader
      .scanf_main
      libc.src.__support.arg_list
      ${file_deps}
    ${use_system_file}
)

add_header_library(
    wide_stream_reader
    HDRS
      wide_stream_reader.h
    DEPENDS
      .reader
      libc.hdr.types.wchar_t
      libc.src.__support.File.file
      libc.src.__support.wchar.character_converter
      libc.src.__support.wchar.mbstate
)

add_header_library(
    vfwscanf_internal
    HDRS
      vfwscanf_internal.h
    DEPENDS
      .wide_stream_reader
      .scanf_main
      libc.hdr.types.FILE
      libc.src.__support.File.file
      libc.src.__support.arg_list
)
