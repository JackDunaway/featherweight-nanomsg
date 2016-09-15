/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright 2016 Garrett D'Amore <garrett@damore.org>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#ifndef NN_ERR_INCLUDED
#define NN_ERR_INCLUDED

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*  Include nn.h header to define nanomsg-specific error codes. */
#include "../nn.h"

#if defined _MSC_VER
#define NN_NORETURN __declspec(noreturn)
#elif defined __GNUC__
#define NN_NORETURN __attribute__ ((noreturn))
#else
#define NN_NORETURN
#endif

/*  Same as system assert(). However, under Win32 assert has some deficiencies.
    Thus this macro. */
#define nn_assert(x) \
    do {\
        if (!(x)) {\
            fprintf (stderr, "Assertion failed: %s\n(%s:%d)\n", #x, \
                __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Unconditionally asserts when a code path defined as unreachable is executed
    at runtime. Such a failure indicates a library developer logic error, such
    as an unexpected value encountered within a function that does not satisfy
    a precondition and should have been prevented with an error return code at
    runtime. */
#define nn_assert_unreachable(reason) \
    do {\
        fprintf (stderr, "Assertion failed: %s\n(%s:%d)\n", #reason, \
            __FILE__, __LINE__);\
        nn_err_abort ();\
    } while (0)

#define nn_assert_state(obj, state_name) \
    do {\
        if ((obj)->state != state_name) {\
            fprintf (stderr, \
                "Assertion failed: %d == %s\n(%s:%d)\n", \
                (obj)->state, #state_name, \
                __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether memory allocation was successful. */
#define nn_assert_alloc(x) \
    do {\
        if (!(x)) {\
            fprintf (stderr, "Out of memory\n(%s:%d)\n",\
                __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Check the condition. If false prints out the errno. */
#define errno_assert(x) \
    do {\
        if (!(x)) {\
            fprintf (stderr, "%s [%d]\n(%s:%d)\n", nn_err_strerror (errno),\
                (int) errno, __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether supplied errno number is an error. */
#define errnum_assert(cond, err) \
    do {\
        if (!(cond)) {\
            fprintf (stderr, "%s [%d]\n(%s:%d)\n", nn_err_strerror (err),\
                (int) (err), __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether the condition is true and an error is indeed present. */
#define nn_assert_is_error(cond, code) \
    do {\
        if (!(cond) || errno != code) {\
            fprintf (stderr,\
                "Expected %s and errno [%s=%d], yet errno is [%d]\n(%s:%d)\n",\
                #cond, #code, (int) (code), errno, __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks the condition. If false prints out the WSAGetLastError info. Note
    that it is assumed that `WSAGetLastError()` covers the range of errors
    expected also from `GetLastError()` within this library. */
#define nn_assert_win(x) \
    do {\
        if (!(x)) {\
            int rc;\
            char errstr [256];\
            DWORD errnum = WSAGetLastError ();\
            rc = FormatMessageA (\
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,\
                NULL, (DWORD) errnum,\
                MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),\
                errstr, (DWORD) sizeof (errstr), NULL );\
            nn_assert (rc);\
            fprintf (stderr, "%s [%d]\n(%s:%d)\n",\
                errstr, (int) errnum, __FILE__, __LINE__);\
            nn_err_abort ();\
        }\
    } while (0)

/*   */
#define nn_assert_unreachable_fsm(state, type)\
    do {\
        fprintf (stderr, "Unexpected FSM state: 0x%08x\n(%s:%d)\n",\
            ((state) | (type)), __FILE__, __LINE__);\
        nn_err_abort ();\
    } while (0)

/*  Compile-time assert. */
#define CT_ASSERT_HELPER2(prefix, line) prefix##line
#define CT_ASSERT_HELPER1(prefix, line) CT_ASSERT_HELPER2(prefix, line)
#if defined __COUNTER__
#define CT_ASSERT(x) \
    typedef int CT_ASSERT_HELPER1(ct_assert_,__COUNTER__) [(x) ? 1 : -1]
#else
#define CT_ASSERT(x) \
    typedef int CT_ASSERT_HELPER1(ct_assert_,__LINE__) [(x) ? 1 : -1]
#endif

/*  Platform-independent wrapper around `abort()`. */
NN_NORETURN void nn_err_abort (void);

/*  Platform-independent low-level ability to clear `errno`. The intended usage
    is more for testing to ensure the library adheres to its contract to set
    errno, and not necessarily intended for library development. */
void nn_clear_errno (void);

/*  Converts an error code into a human-readable string. */
const char *nn_err_strerror (int errnum);

/*  Prints a stack trace. */
void nn_backtrace_print (void);

#ifdef NN_HAVE_WINDOWS
/*  Convert Windows WSA return code to a POSIX error code. */
int nn_err_wsa_to_posix (int wsaerr);
#endif

#endif
