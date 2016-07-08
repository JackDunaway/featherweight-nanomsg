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

#include "fast.h"

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
        if (nn_slow (!(x))) {\
            nn_backtrace_print (); \
            fprintf (stderr, "Assertion failed: %s (%s:%d)\n", #x, \
                __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Unconditionally asserts when a code path defined as unreachable is executed
    at runtime. Such a failure indicates a library developer logic error. */
#define nn_assert_unreachable(reason) \
    do {\
        nn_backtrace_print (); \
        fprintf (stderr, "Assertion failed: %s (%s:%d)\n", #reason, \
            __FILE__, __LINE__);\
        fflush (stderr);\
        nn_err_abort ();\
    } while (0)

#define nn_assert_state(obj, state_name) \
    do {\
        if (nn_slow ((obj)->state != state_name)) {\
            nn_backtrace_print (); \
            fprintf (stderr, \
                "Assertion failed: %d == %s (%s:%d)\n", \
                (obj)->state, #state_name, \
                __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether memory allocation was successful. */
#define alloc_assert(x) \
    do {\
        if (nn_slow (!x)) {\
            nn_backtrace_print (); \
            fprintf (stderr, "Out of memory (%s:%d)\n",\
                __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Check the condition. If false prints out the errno. */
#define errno_assert(x) \
    do {\
        if (nn_slow (!(x))) {\
            nn_backtrace_print (); \
            fprintf (stderr, "%s [%d] (%s:%d)\n", nn_err_strerror (errno),\
                (int) errno, __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether supplied errno number is an error. */
#define errnum_assert(cond, err) \
    do {\
        if (nn_slow (!(cond))) {\
            nn_backtrace_print (); \
            fprintf (stderr, "%s [%d] (%s:%d)\n", nn_err_strerror (err),\
                (int) (err), __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Checks whether the condition is true and an error is indeed present. */
#define nn_assert_is_error(cond, code) \
    do {\
        if (nn_slow (!(cond) || nn_errno () != code)) {\
            nn_backtrace_print ();\
            fprintf (stderr,\
                "Expected %s and errno [%s=%d], yet errno is [%d] (%s:%d)\n",\
                #cond, #code, (int) (code), nn_errno (), __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/* Checks the condition. If false prints out the GetLastError info. */
#define win_assert(x) \
    do {\
        if (nn_slow (!(x))) {\
            char errstr [256];\
            DWORD errnum = WSAGetLastError ();\
            nn_backtrace_print (); \
            nn_win_error ((int) errnum, errstr, 256);\
            fprintf (stderr, "%s [%d] (%s:%d)\n",\
                errstr, (int) errnum, __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/* Checks the condition. If false prints out the WSAGetLastError info. */
#define wsa_assert(x) \
    do {\
        if (nn_slow (!(x))) {\
            char errstr [256];\
            DWORD errnum = WSAGetLastError ();\
            nn_backtrace_print (); \
            nn_win_error (errnum, errstr, 256);\
            fprintf (stderr, "%s [%d] (%s:%d)\n",\
                errstr, (int) errnum, __FILE__, __LINE__);\
            fflush (stderr);\
            nn_err_abort ();\
        }\
    } while (0)

/*  Assertion-like macros for easier fsm debugging. */
#define nn_fsm_error(message, state, src, type) \
    do {\
        nn_backtrace_print(); \
        fprintf (stderr, "%s: state=%d source=%d action=%d (%s:%d)\n", \
            message, state, src, type, __FILE__, __LINE__);\
        fflush (stderr);\
        nn_err_abort ();\
    } while (0)

#define nn_fsm_bad_action(state, src, type) nn_fsm_error(\
    "Unexpected action", state, src, type)
#define nn_fsm_bad_state(state, src, type) nn_fsm_error(\
    "Unexpected state", state, src, type)
#define nn_fsm_bad_source(state, src, type) nn_fsm_error(\
    "Unexpected source", state, src, type)

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

/*  Platform-independent low-level wrapper around `errno`. */
int nn_err_errno (void);

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

/*  Prints a human-readable message (Windows only). */
void nn_win_error (int err, char *buf, size_t bufsize);
#endif

#endif
