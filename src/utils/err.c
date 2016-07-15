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

#include "err.h"

#include <stdlib.h>

#ifndef NN_BACKTRACE_DEPTH
#define NN_BACKTRACE_DEPTH 50
#endif

#if defined NN_HAVE_WINDOWS
#include "win.h"
/*  This warning is disabled to suppress a innocuous but verbose bug when
    compiling with with MSVS2015. This suppression may be removed once a MSVS
    patch is released enabling this solution to build without two such
    occurences of this 4091 warning per project. -JRD, 2016-07-11 */
#pragma warning (push)
#pragma warning (disable:4091)
#include <DbgHelp.h>
#pragma warning (pop)

#ifndef NN_BACKTRACE_MAX_SYMBOL_LEN
#define NN_BACKTRACE_MAX_SYMBOL_LEN 255
#endif

void nn_backtrace_print (void)
{
    void *frames [NN_BACKTRACE_DEPTH];
    USHORT num_captured;
    HANDLE proc;
    SYMBOL_INFO *info;
    char buf [sizeof (*info) + sizeof (CHAR) * (NN_BACKTRACE_MAX_SYMBOL_LEN + 1)];
    IMAGEHLP_LINE64 line;
    DWORD disp;
    DWORD rc;
    BOOL brc;
    int i;

    proc = GetCurrentProcess ();
    brc = SymInitialize (proc, NULL, TRUE);
    if (brc != TRUE)
        return;

    /*  Skip printing information for call to this function itself. */
    num_captured = CaptureStackBackTrace (1, NN_BACKTRACE_DEPTH, frames, NULL);
    if (num_captured == 0)
        return;

    fprintf (stderr, "\n---Begin Stack Trace---\n");

    info = (SYMBOL_INFO *) buf;
    for (i = 0; i < num_captured - 1; i++) {
        memset (info, 0, sizeof (buf) / sizeof (buf [0]));
        /*  Leave room for NULL terminator. */
        info->MaxNameLen = NN_BACKTRACE_MAX_SYMBOL_LEN;
        info->SizeOfStruct = sizeof (*info);
        brc = SymFromAddr (proc, (DWORD64) frames [i], NULL, info);
        if (brc != TRUE) {
            rc = GetLastError ();
            switch (rc) {
            case ERROR_INVALID_ADDRESS:
                /*  No symbol available; continue printing backtrace. */
                fprintf (stderr, "%02d: 0x%p\n", num_captured - i - 1,
                    frames [i]);
                continue;
            default:
                /*  Unknown error; stop printing backtrace. */
                return;
            }
        }
        memset (&line, 0, sizeof (line));
        line.SizeOfStruct = sizeof (line);
        brc = SymGetLineFromAddr64 (proc, (DWORD64) frames [i], &disp, &line);
        if (brc != TRUE) {
            rc = GetLastError ();
            switch (rc) {
            case ERROR_INVALID_ADDRESS:
                /*  No source file available; continue printing backtrace. */
                fprintf (stderr, "%02d: %s\n", num_captured - i - 1,
                    info->Name);
                continue;
            default:
                /*  Unknown error; stop printing backtrace. */
                return;
            }
        }
        /*  Successfully retrieved backtrace symbol and source. */
        fprintf (stderr, "%02d: %s\n(%s:%d)\n", num_captured - i - 1,
            info->Name, line.FileName, line.LineNumber);
    }
    fprintf (stderr, "---End Stack Trace---\n");
}

#elif defined NN_HAVE_BACKTRACE
#include <execinfo.h>

void nn_backtrace_print (void)
{
    void *frames [NN_BACKTRACE_DEPTH];
    int size;
    size = backtrace (frames, NN_BACKTRACE_DEPTH);

    fprintf (stderr, "\n---Begin Stack Trace---\n");
    if (size > 1) {
        /*  Don't include the frame nn_backtrace_print itself. */
        backtrace_symbols_fd (&frames[1], size-1, fileno (stderr));
    }
    fprintf (stderr, "---End Stack Trace---\n");
}

#else
void nn_backtrace_print (void)
{
}
#endif

void nn_err_abort (void)
{
    abort ();
}

void nn_clear_errno (void)
{
    errno = 0;
    nn_assert (nn_errno () == 0);
}

const char *nn_err_strerror (int errnum)
{
    switch (errnum) {
#if defined NN_HAVE_WINDOWS
    case ENOTSUP:
        return "Not supported";
    case EPROTONOSUPPORT:
        return "Protocol not supported";
    case ENOBUFS:
        return "No buffer space available";
    case ENETDOWN:
        return "Network is down";
    case EADDRINUSE:
        return "Address in use";
    case EADDRNOTAVAIL:
        return "Address not available";
    case ECONNREFUSED:
        return "Connection refused";
    case EINPROGRESS:
        return "Operation in progress";
    case ENOTSOCK:
        return "Not a socket";
    case EAFNOSUPPORT:
        return "Address family not supported";
    case EPROTO:
        return "Protocol error";
    case EAGAIN:
        return "Resource unavailable, try again";
    case EBADF:
        return "Bad file descriptor";
    case EINVAL:
        return "Invalid argument";
    case EMFILE:
        return "Too many open files";
    case EFAULT:
        return "Bad address";
    case EACCES:
        return "Permission denied";
    case ENETRESET:
        return "Connection aborted by network";
    case ENETUNREACH:
        return "Network unreachable";
    case EHOSTUNREACH:
        return "Host is unreachable";
    case ENOTCONN:
        return "The socket is not connected";
    case EMSGSIZE:
        return "Message too large";
    case ETIMEDOUT:
        return "Timed out";
    case ECONNABORTED:
        return "Connection aborted";
    case ECONNRESET:
        return "Connection reset";
    case ENOPROTOOPT:
        return "Protocol not available";
    case EISCONN:
        return "Socket is connected";
#endif
    case ETERM:
        return "Nanomsg library was terminated";
    case EFSM:
        return "Operation cannot be performed in this state";
    default:
#if defined _MSC_VER
#pragma warning (push)
#pragma warning (disable:4996)
#endif
        return strerror (errnum);
#if defined _MSC_VER
#pragma warning (pop)
#endif
    }
}

#ifdef NN_HAVE_WINDOWS

int nn_err_wsa_to_posix (int wsaerr)
{
    switch (wsaerr) {
    case WSAEINPROGRESS:
        return EAGAIN;
    case WSAEBADF:
        return EBADF;
    case WSAEINVAL:
        return EINVAL;
    case WSAEMFILE:
        return EMFILE;
    case WSAEFAULT:
        return EFAULT;
    case WSAEPROTONOSUPPORT:
        return EPROTONOSUPPORT;
    case WSAENOBUFS:
        return ENOBUFS;
    case WSAENETDOWN:
        return ENETDOWN;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAEAFNOSUPPORT:
        return EAFNOSUPPORT;
    case WSAEACCES:
        return EACCES;
    case WSAENETRESET:
        return ENETRESET;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAEHOSTUNREACH:
        return EHOSTUNREACH;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAEMSGSIZE:
        return EMSGSIZE;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAENOTSOCK:
        return ENOTSOCK;
    case ERROR_BROKEN_PIPE:
        return ECONNRESET;
    case WSAESOCKTNOSUPPORT:
        return ESOCKTNOSUPPORT;
    case ERROR_NOT_CONNECTED:
        return ENOTCONN;
    case ERROR_PIPE_NOT_CONNECTED:
        return ENOTCONN;
    case ERROR_NO_DATA:
        return EPIPE;
    default:
        nn_assert_unreachable ("Unexpected WSA error.");
    }
}

void nn_win_error (int err, char *buf, size_t bufsize)
{
    DWORD rc = FormatMessageA (
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD) err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, (DWORD) bufsize, NULL );
    nn_assert (rc);
}

#endif
