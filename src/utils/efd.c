/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
    Copyright 2016 Garrett D'Amore <garrett@damore.org>
    Copyright (c) 2015-2016 Jack R. Dunaway.  All rights reserved.

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
#include "efd.h"
#include "clock.h"

#if defined NN_USE_EVENTFD

#include "closefd.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

int nn_efd_init (struct nn_efd *self)
{
    int rc;
    int flags;

    self->efd = eventfd (0, EFD_CLOEXEC);
    if (self->efd == -1 && (errno == EMFILE || errno == ENFILE))
        return -EMFILE;
    errno_assert (self->efd != -1);

    flags = fcntl (self->efd, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    rc = fcntl (self->efd, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);

    return 0;
}

void nn_efd_term (struct nn_efd *self)
{
    int fd = self->efd;
    self->efd = -1;
    nn_closefd (fd);
}

void nn_efd_stop (struct nn_efd *self)
{
    nn_efd_signal (self);
}

nn_fd nn_efd_getfd (struct nn_efd *self)
{
    return self->efd;
}

void nn_efd_signal (struct nn_efd *self)
{
    const uint64_t one = 1;
    ssize_t nbytes;
    int fd = self->efd;

    if (fd < 0)
        return;

    nbytes = write (fd, &one, sizeof (one));
    errno_assert (nbytes == sizeof (one));
}

void nn_efd_unsignal (struct nn_efd *self)
{
    uint64_t count;
    int fd = self->efd;

    if (fd < 0)
        return;

    /*  Extract all the signals from the eventfd. */
    ssize_t sz = read (fd, &count, sizeof (count));
    errno_assert (sz >= 0);
    nn_assert (sz == sizeof (count));
}

#elif defined NN_USE_PIPE

#include "closefd.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

int nn_efd_init (struct nn_efd *self)
{
    int rc;
    int flags;
    int p [2];

#if defined NN_HAVE_PIPE2
    rc = pipe2 (p, O_NONBLOCK | O_CLOEXEC);
#else
    rc = pipe (p);
#endif
    if (rc != 0 && (errno == EMFILE || errno == ENFILE))
        return -EMFILE;
    errno_assert (rc == 0);
    self->r = p [0];
    self->w = p [1];

#if !defined NN_HAVE_PIPE2 && defined FD_CLOEXEC
    rc = fcntl (self->r, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
    rc = fcntl (self->w, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif

#if !defined NN_HAVE_PIPE2
    flags = fcntl (self->r, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    rc = fcntl (self->r, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
#endif

    return 0;
}

void nn_efd_term (struct nn_efd *self)
{
    /*  Close the read side first. */
    int fd = self->r;
    self->r = -1;
    nn_closefd (fd);

    fd = self->w;
    self->w = -1;
    nn_closefd (fd);
}

void nn_efd_stop (struct nn_efd *self)
{
    /*  Close the write side, which wakes up pollers with POLLHUP. */
    int fd = self->w;
    self->w = -1;
    nn_closefd (fd);
}

nn_fd nn_efd_getfd (struct nn_efd *self)
{
    return self->r;
}

void nn_efd_signal (struct nn_efd *self)
{
    ssize_t nbytes;
    char c = 101;
    int fd = self->w;

    if (fd < 0)
        return;
    nbytes = write (self->w, &c, 1);
    errno_assert (nbytes != -1);
    nn_assert (nbytes == 1);
}

void nn_efd_unsignal (struct nn_efd *self)
{
    ssize_t nbytes;
    uint8_t buf [16];

    while (1) {
        int fd = self->r;
        if (fd < 0)
            return;
        nbytes = read (fd, buf, sizeof (buf));
        if (nbytes < 0 && errno == EAGAIN)
            nbytes = 0;
        errno_assert (nbytes >= 0);
        if ((size_t) nbytes < sizeof (buf))
            break;
    }
}

#elif defined NN_USE_SOCKETPAIR

#include "closefd.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

int nn_efd_init (struct nn_efd *self)
{
    int rc;
    int flags;
    int sp [2];

#if defined SOCK_CLOEXEC
    rc = socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
#else
    rc = socketpair (AF_UNIX, SOCK_STREAM, 0, sp);
#endif
    if (rc != 0 && (errno == EMFILE || errno == ENFILE))
        return -EMFILE;
    errno_assert (rc == 0);
    self->r = sp [0];
    self->w = sp [1];

#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    rc = fcntl (self->r, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
    rc = fcntl (self->w, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif

    flags = fcntl (self->r, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    rc = fcntl (self->r, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);

    return 0;
}

void nn_efd_stop (struct nn_efd *self)
{
    int fd = self->w;
    self->w = -1;
    nn_closefd (fd);
}

void nn_efd_term (struct nn_efd *self)
{
    int fd = self->r;
    self->r = -1;
    nn_closefd (fd);
    fd = self->w;
    self->w = -1;
    nn_closefd (fd);
}

nn_fd nn_efd_getfd (struct nn_efd *self)
{
    return self->r;
}

void nn_efd_signal (struct nn_efd *self)
{
    ssize_t nbytes;
    char c = 101;
    int fd = self->w;

    if (fd < 0)
        return;
#if defined MSG_NOSIGNAL
    nbytes = send (fd, &c, 1, MSG_NOSIGNAL);
#else
    nbytes = send (fd, &c, 1, 0);
#endif
    errno_assert (nbytes != -1);
    nn_assert (nbytes == 1);
}

void nn_efd_unsignal (struct nn_efd *self)
{
    ssize_t nbytes;
    uint8_t buf [16];

    while (1) {
        int fd = self->r;
        if (fd < 0)
            return;
        nbytes = recv (self->r, buf, sizeof (buf), 0);
        if (nbytes < 0 && errno == EAGAIN)
            nbytes = 0;
        errno_assert (nbytes >= 0);
        if (nbytes < sizeof (buf))
            break;
    }
}

#elif defined NN_USE_WINSOCK

#define NN_EFD_PORT 5907
#define NN_EFD_RETRIES 1000

#include <string.h>
#include <stdint.h>

int nn_efd_init (struct nn_efd *self)
{
    SECURITY_ATTRIBUTES sa = {0};
    SECURITY_DESCRIPTOR sd;
    BOOL brc;
    HANDLE sync;
    DWORD dwrc;
    SOCKET listener;
    int rc;
    struct sockaddr_in addr;
    int addrlen;
    BOOL reuseaddr;
    BOOL nodelay;
    u_long nonblock;
    int i;

    /*  Make the following critical section accessible to everyone. */
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = FALSE;
    brc = InitializeSecurityDescriptor (&sd, SECURITY_DESCRIPTOR_REVISION);
    nn_assert_win (brc);
    brc = SetSecurityDescriptorDacl(&sd, TRUE, (PACL) NULL, FALSE);
    nn_assert_win (brc);
    sa.lpSecurityDescriptor = &sd;

    /*  This function has to be enclosed in a system-wide critical section
        so that two instances of the library don't accidentally create an efd
        crossing the process boundary. */
    sync = CreateMutex (&sa, FALSE, "Global\\nanomsg-port-mutex");
    nn_assert_win (sync != NULL);

    /*  Enter the critical section. If we cannot get the object in 10 seconds
        then something is seriously wrong. Just bail. */
    dwrc = WaitForSingleObject (sync, 10000);
    switch (dwrc) {
    case WAIT_ABANDONED:
    case WAIT_OBJECT_0:
        break;
    case WAIT_TIMEOUT:
        rc = ETIMEDOUT;
        goto wsafail3;
    default:
        rc = nn_err_wsa_to_posix (WSAGetLastError ());
        goto wsafail3;
    }

    /*  Unfortunately, on Windows the only way to send signal to a file
        descriptor (SOCKET) is to create a full-blown TCP connecting on top of
        the loopback interface. */
    self->w = INVALID_SOCKET;
    self->r = INVALID_SOCKET;

    /*  Create listening socket. */
    listener = socket (AF_INET, SOCK_STREAM, 0);
    if (listener == SOCKET_ERROR)
        goto wsafail;
    brc = SetHandleInformation ((HANDLE) listener, HANDLE_FLAG_INHERIT, 0);
    nn_assert_win (brc);

    /*  This prevents subsequent attempts to create a signaler to fail bacause
        of "TCP port in use" problem. */
    reuseaddr = 1;
    rc = setsockopt (listener, SOL_SOCKET, SO_REUSEADDR,
        (char*) &reuseaddr, sizeof (reuseaddr));
    if (rc == SOCKET_ERROR)
        goto wsafail;

    /*  Bind the listening socket to the local port. */
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (NN_EFD_PORT);
    rc = bind (listener, (const struct sockaddr*) &addr, sizeof (addr));
    if (rc == SOCKET_ERROR)
        goto wsafail;

    /*  Start listening for the incomming connections. In normal case we are
        going to accept just a single connection, so backlog buffer of size
        1 is sufficient. */
    rc = listen (listener, 1);
    if (rc == SOCKET_ERROR)
        goto wsafail;

    /*  The following code is in the loop, because windows sometimes delays
        WSAEADDRINUSE error to the `connect` call. But retrying the connection
        works like a charm. Still we want to limit number of retries  */
    for(i = 0; i < NN_EFD_RETRIES; ++i) {

        /*  Create the writer socket. */
        self->w = socket (AF_INET, SOCK_STREAM, 0);
        if (listener == SOCKET_ERROR)
            goto wsafail;
        brc = SetHandleInformation ((HANDLE) self->w, HANDLE_FLAG_INHERIT, 0);
        nn_assert_win (brc);

        /*  Set TCP_NODELAY on the writer socket to make efd as fast as possible.
            There's only one byte going to be written, so batching would not make
            sense anyway. */
        nodelay = 1;
        rc = setsockopt (self->w, IPPROTO_TCP, TCP_NODELAY, (char*) &nodelay,
            sizeof (nodelay));
        if (rc == SOCKET_ERROR)
            goto wsafail;

        /*  Connect the writer socket to the listener socket. */
        rc = connect (self->w, (struct sockaddr*) &addr, sizeof (addr));
        if (rc == SOCKET_ERROR) {
            rc = nn_err_wsa_to_posix (WSAGetLastError ());
            if (rc == EADDRINUSE) {
                rc = closesocket (self->w);
                if (rc == INVALID_SOCKET)
                    goto wsafail;
                continue;
            }
            goto wsafail2;
        }
        break;
    }
    if (i == NN_EFD_RETRIES)
        goto wsafail2;

    for (;;) {

        /*  Accept new incoming connection. */
        addrlen = sizeof (addr);
        self->r = accept (listener, (struct sockaddr*) &addr, &addrlen);
        if (self->r == INVALID_SOCKET || addrlen != sizeof (addr))
            goto wsafail2;

        /*  Check that the connection actually comes from the localhost. */
        if (addr.sin_addr.s_addr == htonl (INADDR_LOOPBACK))
            break;

        /*  If not so, close the connection and try again. */
        rc = closesocket (self->r);
        if (rc == INVALID_SOCKET)
            goto wsafail;
    }

    /*  Listener socket can be closed now as no more connections for this efd
        are going to be established anyway. */
    rc = closesocket (listener);
    if (rc == INVALID_SOCKET)
        goto wsafail;

    /*  Leave the critical section. */
    brc = ReleaseMutex (sync);
    nn_assert_win (brc != 0);
    brc = CloseHandle (sync);
    nn_assert_win (brc != 0);

    /*  Make the receiving socket non-blocking. */
    nonblock = 1;
    rc = ioctlsocket (self->r, FIONBIO, &nonblock);
    nn_assert_win (rc != SOCKET_ERROR);

    /* Initialise the pre-allocated pollset. */
    FD_ZERO (&self->fds);

    return 0;

wsafail:
    rc = nn_err_wsa_to_posix (WSAGetLastError ());
wsafail2:
    brc = ReleaseMutex (sync);
    nn_assert_win (brc != 0);
wsafail3:
    brc = CloseHandle (sync);
    nn_assert_win (brc != 0);
    return -rc;
}

void nn_efd_stop (struct nn_efd *self)
{
    int rc;
    SOCKET s = self->w;
    self->w = INVALID_SOCKET;

    if (s != INVALID_SOCKET) {
        rc = closesocket (s);
        nn_assert_win (rc != INVALID_SOCKET);
    }
}

void nn_efd_term (struct nn_efd *self)
{
    int rc;
    SOCKET s;

    s = self->r;
    self->r = INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
        rc = closesocket (s);
        nn_assert_win (rc != INVALID_SOCKET);
    }
    s = self->w;
    self->w = INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
        rc = closesocket (s);
        nn_assert_win (rc != INVALID_SOCKET);
    }
}

nn_fd nn_efd_getfd (struct nn_efd *self)
{
    return self->r;
}

void nn_efd_signal (struct nn_efd *self)
{
    int rc;
    unsigned char c = 0xec;
    SOCKET s = self->w;

    if (s != INVALID_SOCKET) {
        rc = send (s, (char*) &c, 1, 0);
        nn_assert_win (rc != SOCKET_ERROR);
        nn_assert (rc == 1);
    }
}

void nn_efd_unsignal (struct nn_efd *self)
{
    int rc;
    uint8_t buf [16];

    while (1) {
        if (self->r == INVALID_SOCKET)
            break;
        rc = recv (self->r, (char*) buf, sizeof (buf), 0);
        if (rc == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
            rc = 0;
        nn_assert_win (rc != SOCKET_ERROR);
        if (rc < sizeof (buf))
            break;
    }
}
#else
    #error
#endif

#if defined NN_HAVE_POLL

#include <poll.h>

int nn_efd_wait (struct nn_efd *self, int timeout)
{
    int rc;
    struct pollfd pfd;
    uint64_t expire;

    if (timeout > 0) {
        expire = nn_clock_ms() + timeout;
    } else {
        expire = timeout;
    }

    /*  In order to solve a problem where the poll call doesn't wake up
        when a file is closed, we sleep a maximum of 100 msec.  This is
        a somewhat unfortunate band-aid to prevent hangs caused by a race
        condition involving nn_close.  In the future this code should be
        replaced by a simpler design using condition variables. */
    for (;;) {
        pfd.fd = nn_efd_getfd (self);
        pfd.events = POLLIN;
        if (pfd.fd < 0)
            return -EBADF;

        switch (expire) {
        case 0:
            /* poll once */
            timeout = 0;
            break;

        case (uint64_t)-1:
            /* infinite wait */
            timeout = 100;
            break;

        default:
            /* bounded wait */
            timeout = (int)(expire - nn_clock_ms());
            if (timeout < 0) {
                return -ETIMEDOUT;
            }
            if (timeout > 100) {
                timeout = 100;
            }
        }
        rc = poll (&pfd, 1, timeout);
        if (rc < 0 && errno == EINTR)
            return -EINTR;
        errno_assert (rc >= 0);
        if (rc == 0) {
            if (expire == 0)
                return -ETIMEDOUT;
            if ((expire != (uint64_t)-1) && (expire < nn_clock_ms())) {
                return -ETIMEDOUT;
            }
            continue;
	}
        return 0;
    }
}

#elif defined NN_USE_WINSOCK

int nn_efd_wait (struct nn_efd *self, int timeout)
{
    int rc;
    struct timeval tv;
    SOCKET fd = self->r;


    if (fd == INVALID_SOCKET) {
        return -EBADF;
    }
    FD_SET (fd, &self->fds);

    if (timeout >= 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = timeout % 1000 * 1000;
        rc = select (0, &self->fds, NULL, NULL, &tv);
    }
    else {
        rc = select (0, &self->fds, NULL, NULL, NULL);
    }

    if (rc == 0) {
        return -ETIMEDOUT;
    }

    if (rc == SOCKET_ERROR) {
        rc = nn_err_wsa_to_posix (WSAGetLastError ());
        errno = rc;

        /*  Treat these as a non-fatal errors, typically occuring when the
            socket is being closed from a separate thread during a blocking
            I/O operation. */
        if (rc == EINTR || rc == ENOTSOCK) {
            return self->r == INVALID_SOCKET ? -EBADF : -EINTR;
        }
    }

    nn_assert_win (rc >= 0);
    return 0;
}

#else

    #error

#endif
