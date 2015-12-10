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

# User-defined options.

option (WS_AUTOBAHN_TESTS "Enable Autobahn TestSuite test for WebSocket transport. Required: http://autobahn.ws/testsuite/installation.html" OFF)
option (ENABLE_NANOCAT "Enable building nanocat utility." OFF)
option (NN_STATIC_LIB "Build static library instead of shared library." OFF)
option (ENABLE_GETADDRINFO_A "Enable/disable use of getaddrinfo_a in place of getaddrinfo." ON)

#  Platform checks.

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_definitions (-DNN_HAVE_LINUX)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_definitions (-DNN_HAVE_OSX)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    add_definitions (-DNN_HAVE_WINDOWS)
    add_definitions (-D_CRT_SECURE_NO_WARNINGS)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    add_definitions (-DNN_HAVE_FREEBSD)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "NetBSD")
    add_definitions (-DNN_HAVE_NETBSD)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    add_definitions (-DNN_HAVE_OPENBSD)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Solaris|SunOS")
    add_definitions (-DNN_HAVE_SOLARIS)
    CHECK_LIBRARY_EXISTS (socket socket "" NN_HAVE_SOLARIS_SOCKET)
    CHECK_LIBRARY_EXISTS (nsl gethostbyname "" NN_HAVE_SOLARIS_GETHOSTBYNAME)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "QNX")
    add_definitions (-DNN_HAVE_QNX)
    CHECK_LIBRARY_EXISTS (socket socket "" NN_HAVE_QNX_SOCKET)
else ()
    message (FATAL_ERROR "ERROR: CMake build system is not supported for this platform: ${CMAKE_SYSTEM_NAME}." )
endif ()


if (WIN32)

elseif (UNIX)
    find_package (Threads REQUIRED)
    CHECK_INCLUDE_FILE (netinet/in.h)
    CHECK_INCLUDE_FILE (netdb.h)
    CHECK_INCLUDE_FILE (arpa/inet.h)
    CHECK_INCLUDE_FILE (unistd.h)
    CHECK_INCLUDE_FILE (sys/socket.h)
    CHECK_INCLUDE_FILE (sys/ioctl.h)
    CHECK_LIBRARY_EXISTS (rt clock_gettime "" NN_HAVE_CLOCK_GETTIME)
else ()

endif ()

# Compiler checks.

if (GCC)
    add_definitions (-DNN_HAVE_GCC)
elseif (MINGW)
    message (FATAL_ERROR "ERROR: MinGW is no longer supported." )
endif ()
# The following compiler definitions are set by the nanomsg autotools build, but
# not used anywhere in source or the project.
# NN_HAVE_GCC
# NN_HAVE_ICC
# NN_HAVE_SUN_STUDIO
# NN_HAVE_CLANG
# NN_HAVE_HPACC

# Feature checks.

CHECK_INCLUDE_FILE (stdint.h NN_HAVE_STDINT)

CHECK_FUNCTION_EXISTS (gethrtime NN_HAVE_GETHRTIME)
CHECK_FUNCTION_EXISTS (socketpair NN_HAVE_SOCKETPAIR)
CHECK_FUNCTION_EXISTS (eventfd NN_HAVE_EVENTFD)
CHECK_FUNCTION_EXISTS (pipe NN_HAVE_PIPE)
CHECK_FUNCTION_EXISTS (pipe2 NN_HAVE_PIPE2)
CHECK_FUNCTION_EXISTS (accept4 NN_HAVE_ACCEPT4)

CHECK_FUNCTION_EXISTS (poll NN_HAVE_POLL)
if (NN_HAVE_POLL)
    CHECK_FUNCTION_EXISTS (epoll_create NN_USE_EPOLL)
    if (NOT NN_USE_EPOLL)
        CHECK_FUNCTION_EXISTS (kqueue NN_USE_KQUEUE)
    else ()
        SET (NN_USE_POLL 1)
    endif ()
endif ()

CHECK_LIBRARY_EXISTS (anl getaddrinfo_a "" NN_HAVE_GETADDRINFO_A)
CHECK_LIBRARY_EXISTS (rt sem_wait "" NN_HAVE_SEMAPHORE_RT)
CHECK_LIBRARY_EXISTS (rt pthread "" NN_HAVE_SEMAPHORE_PTHREAD)

if (NN_HAVE_SEMAPHORE_RT OR NN_HAVE_SEMAPHORE_PTHREAD)
    add_definitions (-DNN_HAVE_SEMAPHORE)
endif ()

if (NOT ENABLE_GETADDRINFO_A)
    add_definitions (-DNN_DISABLE_GETADDRINFO_A)
endif ()

if (NN_HAVE_PIPE2 OR NN_HAVE_ACCEPT4 OR NN_HAVE_GETADDRINFO_A)
    add_definitions (-D_GNU_SOURCE)
endif ()

if (NN_HAVE_EVENTFD)
    add_definitions (-DNN_USE_EVENTFD)
elseif (NN_HAVE_PIPE)
    add_definitions (-DNN_USE_PIPE)
elseif (NN_HAVE_SOCKETPAIR)
    add_definitions (-DNN_USE_SOCKETPAIR)
endif ()

# AC_MSG_CHECKING([for CLOCK_MONOTONIC])
# AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#     #include <time.h>
# ]], [[
#     struct timespec ts;
#     clock_gettime(CLOCK_MONOTONIC, &ts);
#     return 0;
# ]])], [
#     AC_MSG_RESULT([yes])
#     AC_DEFINE([NN_HAVE_CLOCK_MONOTONIC])
# ], [
#     AC_MSG_RESULT([no])
# ])

# AC_LINK_IFELSE([AC_LANG_PROGRAM([], [[
#         #include <stdint.h>
#         int main()
#         {
#             volatile uint32_t n = 0;
#             __sync_fetch_and_add (&n, 1);
#             __sync_fetch_and_sub (&n, 1);
#             return 0;
#         }
# ]])], [
#     AC_DEFINE([NN_HAVE_GCC_ATOMIC_BUILTINS])
# ])

# AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
#     [[#include <atomic.h>]],
#     [[uint32_t value; atomic_cas_32 (&value, 0, 0); return 0;]])],[
#     AC_DEFINE([NN_HAVE_ATOMIC_SOLARIS])],[])

# AC_MSG_CHECKING([for msghdr.msg_control])
# AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#     #include <sys/socket.h>
# ]], [[
#     struct msghdr hdr;
#     hdr.msg_control = 0;
#     return 0;
# ]])], [
#     AC_MSG_RESULT([yes])
#     AC_DEFINE([NN_HAVE_MSG_CONTROL])
# ], [
#     AC_MSG_RESULT([no])
# ])

#  Build the library itself.

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
    target_link_libraries (${NAME} ${PROJECT_NAME})
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
set (CPACK_PACKAGE_NAME ${PROJECT_NAME})
include (CPack)
