#
#   Copyright (c) 2012 Martin Sustrik  All rights reserved.
#   Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
#
#   Permission is hereby granted, free of charge, to any person obtaining a copy
#   of this software and associated documentation files (the "Software"),
#   to deal in the Software without restriction, including without limitation
#   the rights to use, copy, modify, merge, publish, distribute, sublicense,
#   and/or sell copies of the Software, and to permit persons to whom
#   the Software is furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included
#   in all copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
#   IN THE SOFTWARE.
#

cmake_minimum_required (VERSION 2.8.12)
include (CheckIncludeFiles)
include (CheckSymbolExists)
include (CheckLibraryExists)
include (CheckCSourceCompiles)

project (nanomsg C)
enable_testing ()

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Determine library versions.

execute_process (COMMAND ./abi_version.sh OUTPUT_VARIABLE NN_ABI_VERSION)
set (ISSUE_REPORT_MSG "Please consider opening an issue at https://github.com/nanomsg/nanomsg with your findings")

# User-defined options.

option (WS_AUTOBAHN_TESTS "Enable Autobahn TestSuite test for WebSocket transport. Required: http://autobahn.ws/testsuite/installation.html" OFF)
option (ENABLE_NANOCAT "Enable building nanocat utility." OFF)
option (NN_STATIC_LIB "Build static library instead of shared library." OFF)
option (ENABLE_GETADDRINFO_A "Enable/disable use of getaddrinfo_a in place of getaddrinfo." ON)

#  Platform checks.

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_compile_options (-DNN_HAVE_LINUX)
    find_package (Threads REQUIRED)
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    add_compile_options (-DNN_HAVE_OSX)
    find_package (Threads REQUIRED)
elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
    SET (NN_HAVE_WINSOCK 1)
    add_compile_options (-DNN_HAVE_WINDOWS)
    add_compile_options (-D_CRT_SECURE_NO_WARNINGS)
elseif (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    add_compile_options (-DNN_HAVE_FREEBSD)
    find_package (Threads REQUIRED)
elseif (CMAKE_SYSTEM_NAME MATCHES "NetBSD")
    add_compile_options (-DNN_HAVE_NETBSD)
    find_package (Threads REQUIRED)
elseif (CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
    add_compile_options (-DNN_HAVE_OPENBSD)
    find_package (Threads REQUIRED)
elseif (CMAKE_SYSTEM_NAME MATCHES "Solaris|SunOS")
    add_compile_options (-DNN_HAVE_SOLARIS)
    find_package (Threads REQUIRED)
    check_library_exists (socket socket "" NN_HAVE_SOLARIS_SOCKET)
    check_library_exists (nsl gethostbyname "" NN_HAVE_SOLARIS_GETHOSTBYNAME)
elseif (CMAKE_SYSTEM_NAME MATCHES "QNX")
    add_compile_options (-DNN_HAVE_QNX)
    find_package (Threads REQUIRED)
    check_library_exists (socket socket "" NN_HAVE_QNX_SOCKET)
else ()
    message (AUTHOR_WARNING "WARNING: This platform may or may not be supported: ${CMAKE_SYSTEM_NAME}")
    message (AUTHOR_WARNING "${ISSUE_REPORT_MSG}")
endif ()

# Compiler checks.

if (CMAKE_C_COMPILER_ID MATCHES "GNU")
    add_compile_options (-DNN_HAVE_GCC)
elseif (CMAKE_C_COMPILER_ID MATCHES "Intel")
    add_compile_options (-DNN_HAVE_ICC)
elseif (CMAKE_C_COMPILER_ID MATCHES "SunPro")
    add_compile_options (-DNN_HAVE_SUN_STUDIO)
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options (-DNN_HAVE_CLANG)
elseif (CMAKE_C_COMPILER_ID MATCHES "HP")
    add_compile_options (-DNN_HAVE_HPACC)
elseif (CMAKE_C_COMPILER_ID MATCHES "MSVC")
    add_compile_options (-DNN_HAVE_MSVC)
elseif (MINGW)
    message (FATAL_ERROR "ERROR: MinGW is officially no longer supported.")
else ()
    message (AUTHOR_WARNING "WARNING: This compiler may or may not be supported: ${CMAKE_C_COMPILER_ID}")
    message (AUTHOR_WARNING "${ISSUE_REPORT_MSG}")
endif ()

check_include_file (stdint.h NN_HAVE_STDINT)
#    CHECK_INCLUDE_FILE (netinet/in.h)
#    CHECK_INCLUDE_FILE (netdb.h)
#    CHECK_INCLUDE_FILE (arpa/inet.h)
#    CHECK_INCLUDE_FILE (unistd.h)
#    CHECK_INCLUDE_FILE (sys/socket.h)
#    CHECK_INCLUDE_FILE (sys/ioctl.h)

check_function_exists (gethrtime NN_HAVE_GETHRTIME)
check_function_exists (socketpair NN_HAVE_SOCKETPAIR)
check_function_exists (eventfd NN_HAVE_EVENTFD)
check_function_exists (pipe NN_HAVE_PIPE)
check_function_exists (pipe2 NN_HAVE_PIPE2)
check_function_exists (accept4 NN_HAVE_ACCEPT4)
check_function_exists (epoll_create NN_HAVE_EPOLL)
check_function_exists (kqueue NN_HAVE_KQUEUE)
check_function_exists (poll NN_HAVE_POLL)

check_library_exists (anl getaddrinfo_a "" NN_HAVE_GETADDRINFO_A)
check_library_exists (rt clock_gettime "" NN_HAVE_CLOCK_GETTIME)
check_library_exists (rt sem_wait "" NN_HAVE_SEMAPHORE_RT)
check_library_exists (pthread sem_wait  "" NN_HAVE_SEMAPHORE_PTHREAD)

if (NN_HAVE_POLL)
    add_compile_options (-DNN_HAVE_POLL)
endif ()

if (NN_HAVE_SEMAPHORE_RT OR NN_HAVE_SEMAPHORE_PTHREAD)
    add_compile_options (-DNN_HAVE_SEMAPHORE)
endif ()

if (NOT ENABLE_GETADDRINFO_A)
    add_compile_options (-DNN_DISABLE_GETADDRINFO_A)
endif ()

if (NN_HAVE_PIPE2 OR NN_HAVE_ACCEPT4 OR NN_HAVE_GETADDRINFO_A)
    add_compile_options (-D_GNU_SOURCE)
endif ()

check_c_source_compiles ("
    #include <time.h>
    int main()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return 0;
    }
" NN_HAVE_CLOCK_MONOTONIC)
if (NN_HAVE_CLOCK_MONOTONIC)
    add_compile_options (-DNN_HAVE_CLOCK_MONOTONIC)
endif ()

check_c_source_compiles ("
    #include <stdint.h>
    int main()
    {
        volatile uint32_t n = 0;
        __sync_fetch_and_add (&n, 1);
        __sync_fetch_and_sub (&n, 1);
        return 0;
    }
" NN_HAVE_GCC_ATOMIC_BUILTINS)
if (NN_HAVE_GCC_ATOMIC_BUILTINS)
    add_compile_options (-DNN_HAVE_GCC_ATOMIC_BUILTINS)
endif ()

check_c_source_compiles ("
    #include <atomic.h>
    int main()
    {
        uint32_t value;
        atomic_cas_32 (&value, 0, 0);
        return 0;
    }
" NN_HAVE_ATOMIC_SOLARIS)
if (NN_HAVE_ATOMIC_SOLARIS)
    add_compile_options (-DNN_HAVE_ATOMIC_SOLARIS)
endif ()

check_c_source_compiles ("
    #include <sys/socket.h>
    int main()
    {
        struct msghdr hdr;
        hdr.msg_control = 0;
        return 0;
    }
" NN_HAVE_MSG_CONTROL)
if (NN_HAVE_MSG_CONTROL)
    add_compile_options (-DNN_HAVE_MSG_CONTROL)
endif ()

add_subdirectory (src)

#  Build the tools

if (ENABLE_NANOCAT)
    add_executable (nanocat tools/nanocat.c tools/options.c)
    target_link_libraries (nanocat nanomsg)
endif ()

#  Build unit tests.

set (all_tests "")

macro (add_libnanomsg_test NAME TIMEOUT)
    list (APPEND all_tests ${NAME})
    add_executable (${NAME} tests/${NAME}.c)
    target_link_libraries (${NAME} nanomsg)
    add_test (NAME ${NAME} COMMAND ${NAME})
    set_tests_properties (${NAME} PROPERTIES TIMEOUT ${TIMEOUT})
endmacro (add_libnanomsg_test)

#  Transport tests.
add_libnanomsg_test (inproc 5)
add_libnanomsg_test (inproc_shutdown 5)
add_libnanomsg_test (ipc 5)
add_libnanomsg_test (ipc_shutdown 30)
add_libnanomsg_test (ipc_stress 5)
add_libnanomsg_test (tcp 5)
add_libnanomsg_test (tcp_shutdown 120)
add_libnanomsg_test (ws 5)
add_libnanomsg_test (tcpmux 5)

#  Protocol tests.
add_libnanomsg_test (pair 5)
add_libnanomsg_test (pubsub 5)
add_libnanomsg_test (reqrep 5)
add_libnanomsg_test (pipeline 5)
add_libnanomsg_test (survey 5)
add_libnanomsg_test (bus 5)

#  Feature tests.
add_libnanomsg_test (async_shutdown 5)
add_libnanomsg_test (block 5)
add_libnanomsg_test (term 5)
add_libnanomsg_test (timeo 5)
add_libnanomsg_test (iovec 5)
add_libnanomsg_test (msg 5)
add_libnanomsg_test (prio 5)
add_libnanomsg_test (poll 5)
add_libnanomsg_test (device 5)
add_libnanomsg_test (device4 5)
add_libnanomsg_test (device5 5)
add_libnanomsg_test (device6 5)
add_libnanomsg_test (device7 120)
add_libnanomsg_test (emfile 5)
add_libnanomsg_test (domain 5)
add_libnanomsg_test (trie 5)
add_libnanomsg_test (list 5)
add_libnanomsg_test (hash 5)
add_libnanomsg_test (symbol 5)
add_libnanomsg_test (separation 5)
add_libnanomsg_test (zerocopy 5)
add_libnanomsg_test (shutdown 5)
add_libnanomsg_test (cmsg 5)
add_libnanomsg_test (bug328 5)
add_libnanomsg_test (ws_async_shutdown 5)

# Optional tests.
if (WS_AUTOBAHN_TESTS)
    add_libnanomsg_test (ws_stress 60)
    message (STATUS "Autobahn TestSuite for WebSocket is enabled." )
else ()
    message (STATUS "Autobahn TestSuite for WebSocket is disabled." )
endif ()

#  Build the performance tests.

macro (add_libnanomsg_perf NAME)
    add_executable (${NAME} perf/${NAME}.c)
    target_link_libraries (${NAME} nanomsg)
endmacro (add_libnanomsg_perf)

add_libnanomsg_perf (inproc_lat)
add_libnanomsg_perf (inproc_thr)
add_libnanomsg_perf (local_lat)
add_libnanomsg_perf (remote_lat)
add_libnanomsg_perf (local_thr)
add_libnanomsg_perf (remote_thr)

#  NSIS package

install (FILES src/nn.h DESTINATION include/nanomsg)
install (FILES src/inproc.h DESTINATION include/nanomsg)
install (FILES src/ipc.h DESTINATION include/nanomsg)
install (FILES src/tcp.h DESTINATION include/nanomsg)
install (FILES src/ws.h DESTINATION include/nanomsg)
install (FILES src/pair.h DESTINATION include/nanomsg)
install (FILES src/pubsub.h DESTINATION include/nanomsg)
install (FILES src/reqrep.h DESTINATION include/nanomsg)
install (FILES src/pipeline.h DESTINATION include/nanomsg)
install (FILES src/survey.h DESTINATION include/nanomsg)
install (FILES src/bus.h DESTINATION include/nanomsg)

if (BUILD_NANOCAT)
    install (TARGETS nanocat RUNTIME DESTINATION bin)
endif()

set (CPACK_GENERATOR "NSIS")
set (CPACK_PACKAGE_NAME nanomsg)
include (CPack)

# Output all variables for debugging CMake script
get_cmake_property (_variableNames VARIABLES)
foreach (_variableName ${_variableNames})
    message (STATUS "${_variableName}=${${_variableName}}")
endforeach ()
