/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#include "ipc.h"

#include "../../nn.h"

#include "../../ipc.h"

#include "../stream/ustream.h"
#include "../stream/bstream.h"
#include "../stream/cstream.h"

#include "../../aio/fsm.h"
#include "../../aio/worker.h"

#include "../../utils/err.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/cont.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/*  Types of messages passed via IPC transport. */
#define NN_SIPC_MSG_NORMAL 1
#define NN_SIPC_MSG_SHMEM 2

/*  The backlog is set relatively high so that there are not too many failed
    connection attempts during re-connection storms. */
#define NN_IPC_LISTEN_BACKLOG 100

/*  Transport-specific socket options. */
struct nn_ipc_optset {
    struct nn_optset base;
    
    /* Win32 Security Attribute */
    void *sec_attr;

    int outbuffersz;
    int inbuffersz;
};

static void nn_ipc_optset_destroy (struct nn_optset *self);
static int nn_ipc_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen);
static int nn_ipc_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct nn_optset_vfptr nn_ipc_optset_vfptr = {
    nn_ipc_optset_destroy,
    nn_ipc_optset_setopt,
    nn_ipc_optset_getopt
};

/*  nn_transport interface. */
static int nn_ipc_bind (void *hint, struct nn_epbase **epbase);
static int nn_ipc_connect (void *hint, struct nn_epbase **epbase);
static struct nn_optset *nn_ipc_optset (void);

static struct nn_transport nn_ipc_vfptr = {
    "ipc",
    NN_IPC,
    NULL,
    NULL,
    nn_ipc_bind,
    nn_ipc_connect,
    nn_ipc_optset,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_ipc = &nn_ipc_vfptr;

struct nn_aipc {
    struct nn_astream base;
};

struct nn_bipc {
    struct nn_bstream base;
};

struct nn_cipc {
    struct nn_cstream base;
};

static int nn_uipc_sent (struct nn_stream *s);
static int nn_uipc_cancel_io (struct nn_stream *s);
static int nn_uipc_start_resolve (struct nn_cstream *cstream);
static int nn_uipc_start_connect (struct nn_cstream *cstream);
static int nn_uipc_start_listen (struct nn_stream *s, struct nn_epbase *e);
static int nn_uipc_tune (struct nn_stream *s, struct nn_epbase *e);
static int nn_uipc_activate (struct nn_astream *as);
static int nn_uipc_close (struct nn_stream *s);

static struct nn_stream_vfptr nn_stream_vfptr_ipc = {
    nn_uipc_sent,
    nn_uipc_cancel_io,
    nn_uipc_start_resolve,
    nn_uipc_start_connect,
    nn_uipc_start_listen,
    nn_uipc_tune,
    nn_uipc_activate,
    nn_uipc_close
};






static int nn_ipc_bind (void *hint, struct nn_epbase **epbase)
{
    struct nn_bipc *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (*self), "bipc");
    nn_assert_alloc (self);

    return nn_bstream_create (&self->base, hint, epbase, &nn_stream_vfptr_ipc);
}

static int nn_ipc_connect (void *hint, struct nn_epbase **epbase)
{
    struct nn_cipc *self;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (*self), "cipc");
    nn_assert_alloc (self);


    return nn_cstream_create (&self->base, hint, epbase, &nn_stream_vfptr_ipc);
}

static struct nn_optset *nn_ipc_optset ()
{
    struct nn_ipc_optset *optset;

    optset = nn_alloc (sizeof (struct nn_ipc_optset), "optset (ipc)");
    nn_assert_alloc (optset);
    optset->base.vfptr = &nn_ipc_optset_vfptr;

    /*  Default values for the IPC options */
    optset->sec_attr = NULL;
    optset->outbuffersz = 4096;
    optset->inbuffersz = 4096;

    return &optset->base;   
}

static void nn_ipc_optset_destroy (struct nn_optset *self)
{
    struct nn_ipc_optset *optset;

    optset = nn_cont (self, struct nn_ipc_optset, base);
    nn_free (optset);
}

static int nn_ipc_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct nn_ipc_optset *optset = nn_cont (self, struct nn_ipc_optset, base);

    if (optvallen < sizeof (int)) {
        return -EINVAL;
    }

    switch (option) {
    case NN_IPC_SEC_ATTR: 
        optset->sec_attr = (void *) optval;
        return 0;
    case NN_IPC_OUTBUFSZ:
        optset->outbuffersz = *(int *) optval;
        return 0;
    case NN_IPC_INBUFSZ:
        optset->inbuffersz = *(int *) optval;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_ipc_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct nn_ipc_optset *optset = nn_cont (self, struct nn_ipc_optset, base);

    switch (option) {
    case NN_IPC_SEC_ATTR: 
        memcpy (optval, &optset->sec_attr, sizeof (optset->sec_attr));
        *optvallen = sizeof (optset->sec_attr);
        return 0;
    case NN_IPC_OUTBUFSZ:
        *(int *) optval = optset->outbuffersz;
        *optvallen = sizeof (int);
        return 0;
    case NN_IPC_INBUFSZ:
        *(int *) optval = optset->inbuffersz;
        *optvallen = sizeof (int);
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

#if defined NN_HAVE_WINDOWS


struct nn_uipc {

    /*  The underlying stream state machine. */
    struct nn_stream stream;

    /*  When accepting new socket, they have to be created with same
        type as the listening socket. Thus, in listening socket we
        have to store its exact type. */
    int domain;
    int type;
    int protocol;

    /*  For NamedPipes, closing an accepted pipe differs from other pipes.
        If the NamedPipe was accepted, this member is set to 1. 0 otherwise. */
    int isaccepted;

    /*  For NamedPipes, we store the address inside the socket. */
    struct sockaddr_un pipename;

    /*  For now we allocate a new buffer for each write to a named pipe. */
    void *pipesendbuf;

    /*  Pointer to the security attribute structure. */
    SECURITY_ATTRIBUTES *sec_attr;

    /*  Out Buffer and In Buffer size. */
    int outbuffersz;
    int inbuffersz;
};

void nn_uipc_init (struct nn_uipc *self, struct nn_fsm *owner)
{
    nn_stream_init (&self->stream, owner, &nn_stream_vfptr_ipc);

    self->domain = -1;
    self->type = -1;
    self->protocol = -1;
    self->isaccepted = 0;

    /*  NamedPipe-related stuff. */
    memset (&self->pipename, 0, sizeof (self->pipename));
    self->pipesendbuf = NULL;
    self->sec_attr = NULL;

    /*  Set default in/out buffer sizes. */
    self->outbuffersz = 4096;
    self->inbuffersz = 4096;
}

void nn_uipc_term (struct nn_uipc *self)
{
    nn_stream_term (&self->stream);
    if (self->pipesendbuf) {
        nn_free (self->pipesendbuf);
    }
}

int nn_uipc_start (struct nn_uipc *self, int domain, int type, int protocol)
{
    /*  Remember the type of the socket. */
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;

    /*  Start the state machine. */
    nn_fsm_start (&self->stream.fsm);

    return 0;
}

int nn_uipc_setsockopt (struct nn_uipc *self, int level, int optname,
    const void *optval, size_t optlen)
{
    /*  NamedPipes aren't sockets. We can't set socket options on them.
        For now we'll ignore the options. */
    return 0;
}

int nn_uipc_bind (struct nn_uipc *self, const struct sockaddr *addr,
    size_t addrlen)
{
    if (addrlen > sizeof (struct sockaddr_un)) {
        return -EINVAL;
    }

    memcpy (&self->pipename, addr, addrlen);

    return 0;
}

int nn_uipc_listen (struct nn_uipc *self, int backlog)
{
    /*  You can start listening only before the socket is connected. */
    nn_assert (self->stream.state == NN_USOCK_STATE_STARTING);

    /*  Notify the state machine. */
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_LISTENING);

    return 0;
}

int nn_uipc_close (struct nn_stream *s)
{
    struct nn_uipc *self = nn_cont (s, struct nn_uipc, stream);
    BOOL brc;

    if (s->fd == NN_INVALID_FD) {
        return -EINVAL;
    }

    if (self->isaccepted) {
        DisconnectNamedPipe ((HANDLE) s->fd);
    }

    brc = CloseHandle ((HANDLE) s->fd);
    nn_assert_win (brc);
    s->fd = NN_INVALID_FD;

    return 0;
}

static int nn_ipc_start_listen (struct nn_stream *s, struct nn_epbase *e)
{
    const char *addr;
    int rc;
    struct sockaddr_storage ss;
    size_t sslen;
    struct sockaddr_un *un;
#if defined NN_HAVE_UNIX_SOCKETS
    int fd;
#endif

    /*  First, create the AF_UNIX address. */
    addr = nn_epbase_getaddr (e);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    nn_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    sslen = sizeof (struct sockaddr_un);
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

    /*  Delete the IPC file left over by eventual previous runs of
    the application. We'll check whether the file is still in use by
    connecting to the endpoint. On Windows plaform, NamedPipe is used
    which does not have an underlying file. */
#if defined NN_HAVE_UNIX_SOCKETS
    fd = socket (AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        rc = fcntl (fd, F_SETFL, O_NONBLOCK);
        errno_assert (rc != -1 || errno == EINVAL);
        rc = connect (fd, (struct sockaddr*) &ss,
            sizeof (struct sockaddr_un));
        if (rc == -1 && errno == ECONNREFUSED) {
            rc = unlink (addr);
            errno_assert (rc == 0 || errno == ENOENT);
        }
        rc = close (fd);
        errno_assert (rc == 0);
    }
#endif

    /*  Start listening for incoming connections. */
    rc = nn_stream_start (s, AF_UNIX, SOCK_STREAM, 0);
    if (rc < 0) {
        return rc;
    }

    rc = nn_stream_bind (s, (struct sockaddr*) &ss, sslen);
    if (rc < 0) {
        nn_stream_stop (s);
        return rc;
    }

    rc = nn_stream_listen (s, NN_IPC_LISTEN_BACKLOG);
    if (rc < 0) {
        nn_stream_stop (s);
        return rc;
    }

    return 0;
}

void nn_uipc_accept (struct nn_uipc *self, struct nn_uipc *listener, struct nn_epbase *epbase)
{
    char fullname [256] = {0};
    HANDLE p;
    nn_fd fd;
    size_t sz;
    DWORD err;
    BOOL brc;
    int rc;

    /*  TODO: EMFILE can be returned here. */
    rc = nn_uipc_start (self, listener->domain, listener->type,
        listener->protocol);
    errnum_assert (rc == 0, -rc);
    nn_fsm_do_now (&listener->stream.fsm, NN_STREAM_START_ACCEPTING);
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_BEING_ACCEPTED);

    /*  Create a fully qualified name for the named pipe. */
    _snprintf (fullname, sizeof (fullname) - 1, "\\\\.\\pipe\\%s",
        listener->pipename.sun_path);

    /*  Get/Set security attribute pointer. */
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_SEC_ATTR, &self->sec_attr, &sz);
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_OUTBUFSZ, &self->outbuffersz, &sz);
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_INBUFSZ, &self->inbuffersz, &sz);

    p = CreateNamedPipeA ((LPCSTR) fullname,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
        PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
        self->outbuffersz, self->inbuffersz, 0, self->sec_attr);

    /* TODO: How to better handle this potential failure mode? */
    nn_assert_win (p != INVALID_HANDLE_VALUE);

    /*  TODO: Reconsider inelegant, improper downcast of HANDLE to SOCKET. */
    fd = (SOCKET) p;

    /*  Associate the socket with a worker thread/completion port. */
    self->isaccepted = 1;
    nn_worker_fd_register (self->stream.worker, &self->stream, fd);

    /*  Initiate the incoming connection. */
    nn_task_io_start (&listener->stream.incoming, NN_STREAM_ACCEPTED);
    brc = ConnectNamedPipe (p, (LPOVERLAPPED) &listener->stream.incoming.olpd);
    err = brc ? ERROR_SUCCESS : GetLastError();

    /*  Pair the two sockets. */
    nn_assert (self->stream.asock == NULL);
    self->stream.asock = &listener->stream.fsm;
    nn_assert (listener->stream.asock == NULL);
    listener->stream.asock = &self->stream.fsm;

    /*  This is the most likely return path with overlapped I/O. */
    if (err == ERROR_IO_PENDING) {

        /*  Asynchronous accept. */

        return;
    }

    /*  Immediate success. */
    if (err == ERROR_PIPE_CONNECTED || err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");

        nn_fsm_do_now (&listener->stream.fsm, NN_STREAM_ACCEPTED);
        return;
    }

    nn_assert_unreachable ("TODO: determine cases when this can fail.");
}

void nn_uipc_connect (struct nn_uipc *self, const struct sockaddr *addr,
    size_t addrlen, struct nn_epbase *epbase)
{
    char fullname [256] = {0};
    nn_fd fd;
    HANDLE p;
    size_t sz;
    DWORD mode;
    BOOL brc;

    /*  Fail if the socket is already connected, closed or such. */
    nn_assert_state (&self->stream, NN_USOCK_STATE_STARTING);

    /*  Notify the state machine that we've started connecting. */
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_CONNECTING);

    /*  Get/Set security attribute pointer. */
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_SEC_ATTR, &self->sec_attr, &sz);
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_OUTBUFSZ, &self->outbuffersz, &sz);
    nn_epbase_getopt (epbase, NN_IPC, NN_IPC_INBUFSZ, &self->inbuffersz, &sz);

    /*  First, create a fully qualified name for the named pipe. */
    _snprintf (fullname, sizeof (fullname) - 1, "\\\\.\\pipe\\%s",
        ((struct sockaddr_un*) addr)->sun_path);

    p = CreateFileA (fullname, GENERIC_READ | GENERIC_WRITE, 0,
        self->sec_attr, OPEN_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

    if (p == INVALID_HANDLE_VALUE) {
        nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
        return;
    }

    mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
    brc = SetNamedPipeHandleState (p, &mode, NULL, NULL);
    if (!brc) {
        CloseHandle (p);
        nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
        return;
    }

    /*  TODO: Reconsider inelegant, improper downcast of HANDLE to SOCKET. */
    fd = (SOCKET) p;

    /*  Associate the socket with a worker thread/completion port. */
    self->isaccepted = 0;
    nn_worker_fd_register (&self->stream, fd);
    nn_task_io_start (&self->stream.outgoing, NN_STREAM_CONNECTED);

    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_CONNECTED);
    return;
}

void nn_uipc_send (struct nn_uipc *self, const struct nn_iovec *iov,
    int iovcnt)
{
    WSABUF wbuf [NN_STREAM_MAX_IOVCNT];
    DWORD err;
    size_t len;
    size_t idx;
    BOOL brc;
    int i;

    /*  Make sure that the socket is actually alive. */
    nn_assert (self->stream.state == NN_USOCK_STATE_ACTIVE);

    /*  Create a WinAPI-style iovec. */
    len = 0;
    nn_assert (iovcnt <= NN_STREAM_MAX_IOVCNT);
    for (i = 0; i != iovcnt; ++i) {
        wbuf [i].buf = (char FAR*) iov [i].iov_base;
        wbuf [i].len = (ULONG) iov [i].iov_len;
        len += iov [i].iov_len;
    }

    /*  Ensure the total buffer size does not exceed size limitation of WriteFile. */
    nn_assert (len <= MAXDWORD);

    nn_assert (!self->pipesendbuf);
    self->pipesendbuf = nn_alloc (len, "named pipe sendbuf");
    nn_assert_alloc (self->pipesendbuf);

    idx = 0;
    for (i = 0; i != iovcnt; ++i) {
        memcpy ((char*)(self->pipesendbuf) + idx, iov [i].iov_base, iov [i].iov_len);
        idx += iov [i].iov_len;
    }

    /*  Start the send operation. */
    nn_task_io_start (&self->stream.outgoing, NN_STREAM_SENT);
    brc = WriteFile ((HANDLE) self->stream.fd, self->pipesendbuf, (DWORD) len, NULL,
        &self->stream.outgoing.olpd);
    err = brc ? ERROR_SUCCESS : GetLastError ();

    /*  This is the most likely return path with overlapped I/O. */
    if (err == ERROR_IO_PENDING) {
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        // yep, we do -- nn_assert_unreachable ("JRD - do we actually hit this?");
        return;
    }

    /*  Set of expected errors. */
    nn_assert_win (err == ERROR_NO_DATA);
    self->stream.err = EINVAL;
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
    return;
}

void nn_uipc_recv (struct nn_uipc *self, void *buf, size_t len)
{
    DWORD err;
    BOOL brc;

    /*  Make sure that the socket is actually alive. */
    nn_assert (self->stream.state == NN_USOCK_STATE_ACTIVE);

    /*  Start the receive operation. */
    nn_assert (len <= MAXDWORD);
    nn_task_io_start (&self->stream.incoming, NN_STREAM_RECEIVED);
    brc = ReadFile ((HANDLE) self->stream.fd, buf, (DWORD) len, NULL,
        &self->stream.incoming.olpd);
    err = brc ? ERROR_SUCCESS : GetLastError ();

    /*  Success. */
    if (err == ERROR_IO_PENDING) {
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");
        return;
    }

    /*  Set of expected errors. */
    nn_assert (err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_BROKEN_PIPE);
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
}

int nn_uipc_sent (struct nn_stream *s)
{
    struct nn_uipc *self = nn_cont (s, struct nn_uipc, stream);

    if (self->pipesendbuf) {
        nn_free (self->pipesendbuf);
        self->pipesendbuf = NULL;
    }

    return 0;
}

static int nn_uipc_activate (struct nn_astream *as)
{
    return 0;
}

static int nn_uipc_cancel_io (struct nn_stream *s)
{
    nn_task_io_cancel (&s->incoming);
    nn_task_io_cancel (&s->outgoing);

    return 1;
}

static void nn_ipc_start_resolve (struct nn_cstream *cstream)
{
    // make this NULL in vfptr table; transports that do not implement a
    // "resolving" function can jump straight to "connecting".
}

static int nn_ipc_start_connect (struct nn_cstream *cs)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;
    int val;
    size_t sz;

    /*  Create the IPC address from the address string. */
    addr = nn_epbase_getaddr (&cs->epbase);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    nn_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

    /*  Try to start the underlying socket. */
    rc = nn_stream_start (&cs->usock, AF_UNIX, SOCK_STREAM, 0);
    if (rc < 0) {
        return rc;
    }

    /*  Set the relevant socket options. */
    sz = sizeof (val);
    nn_epbase_getopt (&cs->epbase, NN_SOL_SOCKET, NN_SNDBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_stream_setsockopt (&cs->usock, SOL_SOCKET, SO_SNDBUF,
        &val, sizeof (val));
    sz = sizeof (val);
    nn_epbase_getopt (&cs->epbase, NN_SOL_SOCKET, NN_RCVBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_stream_setsockopt (&cs->usock, SOL_SOCKET, SO_RCVBUF,
        &val, sizeof (val));

    /*  Start connecting. */
    nn_stream_connect (&cs->usock, (struct sockaddr*) &ss, sizeof (struct sockaddr_un));
}

int nn_ipc_tune (struct nn_stream *s, struct nn_epbase *e)
{
    size_t optsz;
    int opt;

    /*  Set the relevant socket options. */
    optsz = sizeof (opt);
    nn_epbase_getopt (e, NN_SOL_SOCKET, NN_SNDBUF, &opt, &optsz);
    nn_assert (optsz == sizeof (opt));
    nn_stream_setsockopt (s, SOL_SOCKET, SO_SNDBUF, &opt, optsz);
    optsz = sizeof (opt);
    nn_epbase_getopt (e, NN_SOL_SOCKET, NN_RCVBUF, &opt, &optsz);
    nn_assert (optsz == sizeof (opt));
    nn_stream_setsockopt (s, SOL_SOCKET, SO_RCVBUF, &opt, optsz);

    return 0;
}

#else

#include "../utils/usock_posix.c"

#endif
