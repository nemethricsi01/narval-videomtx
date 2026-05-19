cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(narval_videomtx)

# Workaround: picolibc.specs points to a non-existent path on this toolchain installation.
# The actual picolibc headers are one level up from where the specs file expects them.
# By explicitly adding the correct picolibc include path first, picolibc's stdlib.h
# is used instead of the newlib one, avoiding the _mbstate_t link-time conflict.
if(NOT IDF_TARGET STREQUAL "linux")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-sysroot OUTPUT_VARIABLE _gcc_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    get_filename_component(_gcc_root "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_gcc_root "${_gcc_root}" DIRECTORY)
    if(EXISTS "${_gcc_root}/picolibc/include")
        idf_build_set_property(COMPILE_OPTIONS "-isystem${_gcc_root}/picolibc/include" APPEND)
    endif()
endif()

