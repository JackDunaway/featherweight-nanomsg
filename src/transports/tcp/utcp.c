/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2016 Jack R. Dunaway. All rights reserved.

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

#include "utcp.h"

#if defined NN_HAVE_WINDOWS

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#include <stddef.h>
#include <string.h>
#include <limits.h>

#define NN_USOCK_STATE_IDLE 1
#define NN_USOCK_STATE_STARTING 2
#define NN_USOCK_STATE_BEING_ACCEPTED 3
#define NN_USOCK_STATE_ACCEPTED 4
#define NN_USOCK_STATE_CONNECTING 5
#define NN_USOCK_STATE_ACTIVE 6
#define NN_USOCK_STATE_CANCELLING_IO 7
#define NN_USOCK_STATE_DONE 8
#define NN_USOCK_STATE_LISTENING 9
#define NN_USOCK_STATE_ACCEPTING 10
#define NN_USOCK_STATE_CANCELLING 11
#define NN_USOCK_STATE_STOPPING 12
#define NN_USOCK_STATE_STOPPING_ACCEPT 13

#define NN_USOCK_ACTION_ACCEPT 1
#define NN_USOCK_ACTION_BEING_ACCEPTED 2
#define NN_USOCK_ACTION_CANCEL 3
#define NN_USOCK_ACTION_LISTEN 4
#define NN_USOCK_ACTION_CONNECT 5
#define NN_USOCK_ACTION_ACTIVATE 6
#define NN_USOCK_ACTION_DONE 7
#define NN_USOCK_ACTION_ERROR 8

#define NN_USOCK_SRC_IN 1
#define NN_USOCK_SRC_OUT 2

/*  Private functions. */
static void nn_utcp_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_utcp_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_utcp_init (struct nn_utcp *self, int src, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_utcp_handler, nn_utcp_shutdown,
        src, self, owner);
    self->state = NN_USOCK_STATE_IDLE;
    self->s = INVALID_SOCKET;
    nn_worker_op_init (&self->in, NN_USOCK_SRC_IN, &self->fsm);
    nn_worker_op_init (&self->out, NN_USOCK_SRC_OUT, &self->fsm);
    self->domain = -1;
    self->type = -1;
    self->protocol = -1;

    /*  Intialise events raised by usock. */
    nn_fsm_event_init (&self->event_established);
    nn_fsm_event_init (&self->event_sent);
    nn_fsm_event_init (&self->event_received);
    nn_fsm_event_init (&self->event_error);

    /*  No accepting is going on at the moment. */
    self->asock = NULL;
    self->ainfo = NULL;
}

void nn_utcp_term (struct nn_utcp *self)
{
    nn_assert_state (self, NN_USOCK_STATE_IDLE);

    if (self->ainfo)
        nn_free (self->ainfo);
    nn_fsm_event_term (&self->event_error);
    nn_fsm_event_term (&self->event_received);
    nn_fsm_event_term (&self->event_sent);
    nn_fsm_event_term (&self->event_established);
    nn_worker_op_term (&self->out);
    nn_worker_op_term (&self->in);
    nn_fsm_term (&self->fsm);
}

int nn_utcp_start (struct nn_utcp *self, int domain, int type, int protocol)
{
    DWORD only;
    BOOL nodelay;
    int rc;

    /*  Open the underlying socket. */
    self->s = socket (domain, type, protocol);
    if (self->s == INVALID_SOCKET)
        return -nn_err_wsa_to_posix (WSAGetLastError ());

    /*  IPv4 mapping for IPv6 sockets is disabled by default. Switch it on. */
    if (domain == AF_INET6) {
        only = 0;
        rc = setsockopt (self->s, IPPROTO_IPV6, IPV6_V6ONLY,
            (const char*) &only, sizeof (only));
        nn_assert_win (rc != SOCKET_ERROR);
    }

    /*  By default, ensure Nagle's algorithm for batched sends is off. */
    nodelay = TRUE;
    rc = setsockopt (self->s, IPPROTO_TCP, TCP_NODELAY,
        (const char*) &nodelay, sizeof (nodelay));
    nn_assert_win (rc != SOCKET_ERROR);

    /*  Associate the socket with a worker thread/completion port. */
    nn_worker_register_iocp (&self->fsm, (HANDLE) self->s);

    /*  Remember the type of the socket. */
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    return 0;
}

int nn_utcp_setsockopt (struct nn_utcp *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  The socket can be modified only before it's active. */
    nn_assert (self->state == NN_USOCK_STATE_STARTING ||
        self->state == NN_USOCK_STATE_ACCEPTED);

    nn_assert (optlen < INT_MAX);

    rc = setsockopt (self->s, level, optname, (char*) optval, (int) optlen);
    if (rc == SOCKET_ERROR)
        return -nn_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int nn_utcp_bind (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;
    ULONG opt;

    /*  You can set socket options only before the socket is connected. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    /*  On Windows, the bound port can be hijacked
        if SO_EXCLUSIVEADDRUSE is not set. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        (const char*) &opt, sizeof (opt));
    nn_assert_win (rc != SOCKET_ERROR);

    nn_assert (addrlen < INT_MAX);
    rc = bind (self->s, addr, (int) addrlen);
    if (rc == SOCKET_ERROR)
        return -nn_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int nn_utcp_listen (struct nn_utcp *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    rc = listen (self->s, backlog);
    if (rc == SOCKET_ERROR) {
        return -nn_err_wsa_to_posix (WSAGetLastError ());
    }

    /*  Notify the state machine. */
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_LISTEN);

    return 0;
}

void nn_utcp_accept (struct nn_utcp *self, struct nn_utcp *listener)
{
    DWORD nbytes;
    BOOL brc;
    int err;
    int rc;

    /*  TODO: EMFILE can be returned here. */
    rc = nn_utcp_start (self, listener->domain, listener->type,
        listener->protocol);
    errnum_assert (rc == 0, -rc);
    nn_fsm_action (&listener->fsm, NN_USOCK_ACTION_ACCEPT);
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_BEING_ACCEPTED);

    /*  If the memory for accept information is not yet allocated, do so. */
    if (!listener->ainfo) {
        listener->ainfo = nn_alloc (512, "accept info");
        nn_assert_alloc (listener->ainfo);
    }

    /*  Wait for the incoming connection. */
    memset (&listener->in.olpd, 0, sizeof (listener->in.olpd));
    brc = AcceptEx (listener->s, self->s, listener->ainfo, 0, 256, 256, &nbytes,
        &listener->in.olpd);
    err = brc ? ERROR_SUCCESS : WSAGetLastError();

    /*  This is the most likely return path with overlapped I/O. */
    if (err == WSA_IO_PENDING) {

        /*  Pair the two sockets. */
        nn_assert (!self->asock);
        self->asock = listener;
        nn_assert (!listener->asock);
        listener->asock = self;

        /*  Asynchronous accept. */
        nn_worker_op_start (&listener->in, 0);

        return;
    }

    /*  Immediate success. */
    nn_assert (err == ERROR_SUCCESS);

    nn_fsm_action (&listener->fsm, NN_USOCK_ACTION_DONE);
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_DONE);

    return;
}

void nn_utcp_connect (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen)
{
    GUID fid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pconnectex;
    BOOL brc;
    DWORD nbytes;
    int err;

    /*  Fail if the socket is already connected, closed or such. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    /*  Notify the state machine that we've started connecting. */
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_CONNECT);

    /*  Get the pointer to connect function. */
    brc = WSAIoctl (self->s, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &fid, sizeof (fid), &pconnectex, sizeof (pconnectex),
        &nbytes, NULL, NULL) == 0;
    nn_assert_win (brc == TRUE && nbytes == sizeof (pconnectex));

    /*  Ensure it is safe to cast this value to what might be a smaller
        integer type to conform to the pconnectex function signature. */
    nn_assert (addrlen < INT_MAX);

    /*  Connect itself. */
    memset (&self->out.olpd, 0, sizeof (self->out.olpd));
    brc = pconnectex (self->s, addr, (int) addrlen, NULL, 0, NULL,
        &self->out.olpd);
    err = brc ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Most likely return path is asynchronous connect. */
    if (err == WSA_IO_PENDING) {
        nn_worker_op_start (&self->out, 0);
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_DONE);
        return;
    }

    /*  Unkown error. */
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
    return;
}

void nn_utcp_send (struct nn_utcp *self, const struct nn_iovec *iov,
    int iovcnt)
{
    WSABUF wbuf [NN_UTCP_MAX_IOVCNT];
    DWORD err;
    size_t len;
    int rc;
    int i;

    /*  Make sure that the socket is actually alive. */
    nn_assert_state (self, NN_USOCK_STATE_ACTIVE);

    /*  Create a WinAPI-style iovec. */
    len = 0;
    nn_assert (iovcnt <= NN_UTCP_MAX_IOVCNT);
    for (i = 0; i != iovcnt; ++i) {
        wbuf [i].buf = (char FAR*) iov [i].iov_base;
        wbuf [i].len = (ULONG) iov [i].iov_len;
        len += iov [i].iov_len;
    }

    /*  Start the send operation. */
    memset (&self->out.olpd, 0, sizeof (self->out.olpd));
    rc = WSASend (self->s, wbuf, iovcnt, NULL, 0, &self->out.olpd, NULL);
    err = (rc == 0) ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Success. */
    if (err == ERROR_SUCCESS || err == WSA_IO_PENDING) {

        /// testing
        nn_assert (err == ERROR_SUCCESS);


        nn_worker_op_start (&self->out, 0);
        return;
    }

    /*  Set of expected errors. */
    nn_assert_win (err == WSAECONNABORTED || err == WSAECONNRESET ||
        err == WSAENETDOWN || err == WSAENETRESET ||
        err == WSAENOTCONN || err == WSAENOBUFS || err == WSAEWOULDBLOCK);
    self->errnum = nn_err_wsa_to_posix (err);
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
}

void nn_utcp_recv (struct nn_utcp *self, void *buf, size_t len)
{
    DWORD wflags;
    WSABUF wbuf;
    int err;
    int rc;

    /*  Make sure that the socket is actually alive. */
    nn_assert_state (self, NN_USOCK_STATE_ACTIVE);

    /*  Start the receive operation. */
    wbuf.len = (ULONG) len;
    wbuf.buf = (char FAR*) buf;
    wflags = MSG_WAITALL;
    memset (&self->in.olpd, 0, sizeof (self->in.olpd));
    rc = WSARecv (self->s, &wbuf, 1, NULL, &wflags, &self->in.olpd, NULL);
    err = (rc == 0) ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Success. */
    if (err == ERROR_SUCCESS || err == WSA_IO_PENDING) {
        nn_worker_op_start (&self->in, 1);
        return;
    }

    /*  Set of expected errors. */
    nn_assert (err == WSAECONNABORTED || err == WSAECONNRESET ||
        err == WSAEDISCON || err == WSAENETDOWN || err == WSAENETRESET ||
        err == WSAENOTCONN || err == WSAETIMEDOUT || err == WSAEWOULDBLOCK);
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
}

/*  Returns 0 if there's nothing to cancel or 1 otherwise. */
static int nn_utcp_cancel_io (struct nn_utcp *self)
{
    int rc;
    BOOL brc;

    /*  For some reason simple CancelIo doesn't seem to work here.
        We have to use CancelIoEx instead. */
    rc = 0;
    if (!nn_worker_op_isidle (&self->in)) {
        brc = CancelIoEx ((HANDLE) self->s, &self->in.olpd);
        nn_assert_win (brc || GetLastError () == ERROR_NOT_FOUND);
        rc = 1;
    }
    if (!nn_worker_op_isidle (&self->out)) {
        brc = CancelIoEx ((HANDLE) self->s, &self->out.olpd);
        nn_assert_win (brc || GetLastError () == ERROR_NOT_FOUND);
        rc = 1;
    }

    return rc;
}

static void nn_utcp_close (struct nn_utcp *self)
{
    int rc;
    
    rc = closesocket (self->s);
    self->s = INVALID_SOCKET;
    nn_assert_win (rc == 0);
}

static void nn_utcp_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_utcp *usock;

    usock = nn_cont (self, struct nn_utcp, fsm);

    if (src == NN_FSM_ACTION && type == NN_FSM_STOP) {

        /*  Socket in ACCEPTING state cannot be closed.
            Stop the socket being accepted first. */
        nn_assert (usock->state != NN_USOCK_STATE_ACCEPTING);

        /*  Synchronous stop. */
        if (usock->state == NN_USOCK_STATE_IDLE)
            goto finish3;
        if (usock->state == NN_USOCK_STATE_DONE)
            goto finish2;
        if (usock->state == NN_USOCK_STATE_STARTING ||
            usock->state == NN_USOCK_STATE_ACCEPTED ||
            usock->state == NN_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (usock->state == NN_USOCK_STATE_BEING_ACCEPTED) {
            nn_fsm_action (&usock->asock->fsm, NN_USOCK_ACTION_CANCEL);
            usock->state = NN_USOCK_STATE_STOPPING_ACCEPT;
            return;
        }

        /*  If we were already in the process of cancelling overlapped
            operations, we don't have to do anything. Continue waiting
            till cancelling is finished. */
        if (usock->state == NN_USOCK_STATE_CANCELLING_IO) {
            usock->state = NN_USOCK_STATE_STOPPING;
            return;
        }

        /*  Notify our parent that pipe socket is shutting down  */
        nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_SHUTDOWN);

        /*  In all remaining states we'll simply cancel all overlapped
            operations. */
        if (nn_utcp_cancel_io (usock) == 0)
            goto finish1;
        usock->state = NN_USOCK_STATE_STOPPING;
        return;
    }
    if (usock->state == NN_USOCK_STATE_STOPPING_ACCEPT) {
        nn_assert (src == NN_FSM_ACTION && type == NN_USOCK_ACTION_DONE);
        goto finish1;
    }
    if (usock->state == NN_USOCK_STATE_STOPPING) {
        if (!nn_worker_op_isidle (&usock->in) ||
            !nn_worker_op_isidle (&usock->out))
            return;
    finish1:
        nn_utcp_close (usock);
    finish2:
        usock->state = NN_USOCK_STATE_IDLE;
        nn_fsm_stopped (&usock->fsm, NN_STREAM_STOPPED);
    finish3:
        return;
    }

    nn_fsm_bad_state(usock->state, src, type);
}

static void nn_utcp_handler (struct nn_fsm *self, int src, int type,
    void *srcptr)
{
    struct nn_utcp *usock;

    usock = nn_cont (self, struct nn_utcp, fsm);

    switch (usock->state) {

/*****************************************************************************/
/*  IDLE state.                                                              */
/*****************************************************************************/
    case NN_USOCK_STATE_IDLE:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                usock->state = NN_USOCK_STATE_STARTING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  STARTING state.                                                          */
/*****************************************************************************/
    case NN_USOCK_STATE_STARTING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_LISTEN:
                usock->state = NN_USOCK_STATE_LISTENING;
                return;
            case NN_USOCK_ACTION_CONNECT:
                usock->state = NN_USOCK_STATE_CONNECTING;
                return;
            case NN_USOCK_ACTION_BEING_ACCEPTED:
                usock->state = NN_USOCK_STATE_BEING_ACCEPTED;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  BEING_ACCEPTED state.                                                    */
/*****************************************************************************/
    case NN_USOCK_STATE_BEING_ACCEPTED:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_ACCEPTED;
                nn_fsm_raise (&usock->fsm, &usock->event_established,
                    NN_STREAM_ACCEPTED);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  ACCEPTED state.                                                          */
/*****************************************************************************/
    case NN_USOCK_STATE_ACCEPTED:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ACTIVATE:
                usock->state = NN_USOCK_STATE_ACTIVE;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CONNECTING state.                                                        */
/*****************************************************************************/
    case NN_USOCK_STATE_CONNECTING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_ACTIVE;
                nn_fsm_raise (&usock->fsm, &usock->event_established,
                    NN_STREAM_CONNECTED);
                return;
            case NN_USOCK_ACTION_ERROR:
                nn_utcp_close (usock);
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_OUT:
            switch (type) {
            case NN_WORKER_OP_DONE:
                usock->state = NN_USOCK_STATE_ACTIVE;
                nn_fsm_raise (&usock->fsm, &usock->event_established,
                    NN_STREAM_CONNECTED);
                return;
            case NN_WORKER_OP_ERROR:
                nn_utcp_close (usock);
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  ACTIVE state.                                                            */
/*****************************************************************************/
    case NN_USOCK_STATE_ACTIVE:
        switch (src) {
        case NN_USOCK_SRC_IN:
            switch (type) {
            case NN_WORKER_OP_DONE:
                nn_fsm_raise (&usock->fsm, &usock->event_received,
                    NN_STREAM_RECEIVED);
                return;
            case NN_WORKER_OP_ERROR:
                if (nn_utcp_cancel_io (usock) == 0) {
                    nn_fsm_raise(&usock->fsm, &usock->event_error,
                        NN_STREAM_ERROR);
                    nn_utcp_close (usock);
                    usock->state = NN_USOCK_STATE_DONE;
                    return;
                }
                usock->state = NN_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_OUT:
            switch (type) {
            case NN_WORKER_OP_DONE:
                nn_fsm_raise (&usock->fsm, &usock->event_sent, NN_STREAM_SENT);
                return;
            case NN_WORKER_OP_ERROR:
                if (nn_utcp_cancel_io (usock) == 0) {
                    nn_fsm_raise(&usock->fsm, &usock->event_error,
                        NN_STREAM_ERROR);
                    nn_utcp_close (usock);
                    usock->state = NN_USOCK_STATE_DONE;
                    return;
                }
                usock->state = NN_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ERROR:
                if (nn_utcp_cancel_io (usock) == 0) {
                    nn_fsm_raise(&usock->fsm, &usock->event_error,
                        NN_STREAM_SHUTDOWN);
                    nn_utcp_close (usock);
                    usock->state = NN_USOCK_STATE_DONE;
                    return;
                }
                usock->state = NN_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CANCELLING_IO state.                                                     */
/*****************************************************************************/
    case NN_USOCK_STATE_CANCELLING_IO:
        switch (src) {
        case NN_USOCK_SRC_IN:
        case NN_USOCK_SRC_OUT:
            if (!nn_worker_op_isidle (&usock->in) ||
                !nn_worker_op_isidle (&usock->out))
                return;
            nn_fsm_raise(&usock->fsm, &usock->event_error, NN_STREAM_SHUTDOWN);
            nn_utcp_close (usock);
            usock->state = NN_USOCK_STATE_DONE;
            return;
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  DONE state.                                                              */
/*****************************************************************************/
    case NN_USOCK_STATE_DONE:
        nn_fsm_bad_source (usock->state, src, type);

/*****************************************************************************/
/*  LISTENING state.                                                         */
/*****************************************************************************/
    case NN_USOCK_STATE_LISTENING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ACCEPT:
                usock->state = NN_USOCK_STATE_ACCEPTING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  ACCEPTING state.                                                         */
/*****************************************************************************/
    case NN_USOCK_STATE_ACCEPTING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_LISTENING;
                return;
            case NN_USOCK_ACTION_CANCEL:
                nn_utcp_cancel_io (usock);
                usock->state = NN_USOCK_STATE_CANCELLING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_IN:
            switch (type) {
            case NN_WORKER_OP_DONE:

                /*  Adjust the new usock object. */
                usock->asock->state = NN_USOCK_STATE_ACCEPTED;

                /*  Notify the user that connection was accepted. */
                nn_fsm_raise (&usock->asock->fsm,
                    &usock->asock->event_established, NN_STREAM_ACCEPTED);

                /*  Disassociate the listener socket from the accepted
                    socket. */
                usock->asock->asock = NULL;
                usock->asock = NULL;

                /*  Wait till the user starts accepting once again. */
                usock->state = NN_USOCK_STATE_LISTENING;

                return;

            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CANCELLING state.                                                        */
/*****************************************************************************/
    case NN_USOCK_STATE_CANCELLING:
        switch (src) {
        case NN_USOCK_SRC_IN:
            switch (type) {
            case NN_WORKER_OP_DONE:
            case NN_WORKER_OP_ERROR:

                /*  TODO: The socket being accepted should be closed here. */

                usock->state = NN_USOCK_STATE_LISTENING;

                /*  Notify the accepted socket that it was stopped. */
                nn_fsm_action (&usock->asock->fsm, NN_USOCK_ACTION_DONE);

                return;

            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  Invalid state.                                                           */
/*****************************************************************************/
    default:
        nn_fsm_bad_state (usock->state, src, type);
    }
}

#else

#include "../utils/alloc.h"
#include "../utils/closefd.h"
#include "../utils/cont.h"
#include "../utils/err.h"
#include "../utils/attr.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>

#define NN_USOCK_STATE_IDLE 1
#define NN_USOCK_STATE_STARTING 2
#define NN_USOCK_STATE_BEING_ACCEPTED 3
#define NN_USOCK_STATE_ACCEPTED 4
#define NN_USOCK_STATE_CONNECTING 5
#define NN_USOCK_STATE_ACTIVE 6
#define NN_USOCK_STATE_REMOVING_FD 7
#define NN_USOCK_STATE_DONE 8
#define NN_USOCK_STATE_LISTENING 9
#define NN_USOCK_STATE_ACCEPTING 10
#define NN_USOCK_STATE_CANCELLING 11
#define NN_USOCK_STATE_STOPPING 12
#define NN_USOCK_STATE_STOPPING_ACCEPT 13
#define NN_USOCK_STATE_ACCEPTING_ERROR 14

#define NN_USOCK_ACTION_ACCEPT 1
#define NN_USOCK_ACTION_BEING_ACCEPTED 2
#define NN_USOCK_ACTION_CANCEL 3
#define NN_USOCK_ACTION_LISTEN 4
#define NN_USOCK_ACTION_CONNECT 5
#define NN_USOCK_ACTION_ACTIVATE 6
#define NN_USOCK_ACTION_DONE 7
#define NN_USOCK_ACTION_ERROR 8
#define NN_USOCK_ACTION_STARTED 9

#define NN_USOCK_SRC_FD 1
#define NN_USOCK_SRC_TASK_CONNECTING 2
#define NN_USOCK_SRC_TASK_CONNECTED 3
#define NN_USOCK_SRC_TASK_ACCEPT 4
#define NN_USOCK_SRC_TASK_SEND 5
#define NN_USOCK_SRC_TASK_RECV 6
#define NN_USOCK_SRC_TASK_STOP 7

/*  Private functions. */
static void nn_usock_init_from_fd (struct nn_utcp *self, int s);
static int nn_usock_send_raw (struct nn_utcp *self, struct msghdr *hdr);
static int nn_usock_recv_raw (struct nn_utcp *self, void *buf, size_t *len);
static int nn_usock_geterr (struct nn_utcp *self);
static void nn_utcp_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_utcp_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_utcp_init (struct nn_utcp *self, int src, struct nn_fsm *owner)
{
    /*  Initalise the state machine. */
    nn_fsm_init (&self->fsm, nn_utcp_handler, nn_utcp_shutdown,
        src, self, owner);
    self->state = NN_USOCK_STATE_IDLE;

    /*  Choose a worker thread to handle this socket. */
    self->worker = nn_fsm_choose_worker (&self->fsm);

    /*  Actual file descriptor will be generated during 'start' step. */
    self->s = -1;
    self->errnum = 0;

    self->in.buf = NULL;
    self->in.len = 0;
    self->in.batch = NULL;
    self->in.batch_len = 0;
    self->in.batch_pos = 0;
    self->in.pfd = NULL;

    memset (&self->out.hdr, 0, sizeof (struct msghdr));

    /*  Initialise tasks for the worker thread. */
    nn_worker_fd_init (&self->wfd, NN_USOCK_SRC_FD, &self->fsm);
    nn_worker_task_init (&self->task_connecting, NN_USOCK_SRC_TASK_CONNECTING,
        &self->fsm);
    nn_worker_task_init (&self->task_connected, NN_USOCK_SRC_TASK_CONNECTED,
        &self->fsm);
    nn_worker_task_init (&self->task_accept, NN_USOCK_SRC_TASK_ACCEPT,
        &self->fsm);
    nn_worker_task_init (&self->task_send, NN_USOCK_SRC_TASK_SEND, &self->fsm);
    nn_worker_task_init (&self->task_recv, NN_USOCK_SRC_TASK_RECV, &self->fsm);
    nn_worker_task_init (&self->task_stop, NN_USOCK_SRC_TASK_STOP, &self->fsm);

    /*  Intialise events raised by usock. */
    nn_fsm_event_init (&self->event_established);
    nn_fsm_event_init (&self->event_sent);
    nn_fsm_event_init (&self->event_received);
    nn_fsm_event_init (&self->event_error);

    /*  accepting is not going on at the moment. */
    self->asock = NULL;
}

void nn_utcp_term (struct nn_utcp *self)
{
    nn_assert_state (self, NN_USOCK_STATE_IDLE);

    if (self->in.batch)
        nn_free (self->in.batch);

    nn_fsm_event_term (&self->event_error);
    nn_fsm_event_term (&self->event_received);
    nn_fsm_event_term (&self->event_sent);
    nn_fsm_event_term (&self->event_established);

    nn_worker_cancel (self->worker, &self->task_recv);

    nn_worker_task_term (&self->task_stop);
    nn_worker_task_term (&self->task_recv);
    nn_worker_task_term (&self->task_send);
    nn_worker_task_term (&self->task_accept);
    nn_worker_task_term (&self->task_connected);
    nn_worker_task_term (&self->task_connecting);
    nn_worker_fd_term (&self->wfd);

    nn_fsm_term (&self->fsm);
}

int nn_utcp_start (struct nn_utcp *self, int domain, int type, int protocol)
{
    int s;

    /*  If the operating system allows to directly open the socket with CLOEXEC
        flag, do so. That way there are no race conditions. */
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    /* Open the underlying socket. */
    s = socket (domain, type, protocol);
    if (s < 0)
        return -errno;

    nn_usock_init_from_fd (self, s);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    return 0;
}

static void nn_usock_init_from_fd (struct nn_utcp *self, int s)
{
    int rc;
    int opt;

    nn_assert (self->state == NN_USOCK_STATE_IDLE ||
        NN_USOCK_STATE_BEING_ACCEPTED);

    /*  Store the file descriptor. */
    nn_assert (self->s == -1);
    self->s = s;

    /*  Setting FD_CLOEXEC option immediately after socket creation is the
        second best option after using SOCK_CLOEXEC. There is a race condition
        here (if process is forked between socket creation and setting
        the option) but the problem is pretty unlikely to happen. */
#if defined FD_CLOEXEC
    rc = fcntl (self->s, F_SETFD, FD_CLOEXEC);
#if defined NN_HAVE_OSX
    errno_assert (rc != -1 || errno == EINVAL);
#else
    errno_assert (rc != -1);
#endif
#endif

    /*  If applicable, prevent SIGPIPE signal when writing to the connection
        already closed by the peer. */
#ifdef SO_NOSIGPIPE
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof (opt));
#if defined NN_HAVE_OSX
    errno_assert (rc == 0 || errno == EINVAL);
#else
    errno_assert (rc == 0);
#endif
#endif

    /*  Switch the socket to the non-blocking mode. All underlying sockets
        are always used in the callbackhronous mode. */
    opt = fcntl (self->s, F_GETFL, 0);
    if (opt == -1)
        opt = 0;
    if (!(opt & O_NONBLOCK)) {
        rc = fcntl (self->s, F_SETFL, opt | O_NONBLOCK);
#if defined NN_HAVE_OSX
        errno_assert (rc != -1 || errno == EINVAL);
#else
        errno_assert (rc != -1);
#endif
    }
}

void nn_usock_async_stop (struct nn_utcp *self)
{
    nn_worker_execute (self->worker, &self->task_stop);
    nn_fsm_raise (&self->fsm, &self->event_error, NN_STREAM_SHUTDOWN);
}

int nn_utcp_setsockopt (struct nn_utcp *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  The socket can be modified only before it's active. */
    nn_assert (self->state == NN_USOCK_STATE_STARTING ||
        self->state == NN_USOCK_STATE_ACCEPTED);

    /*  EINVAL errors are ignored on OSX platform. The reason for that is buggy
        OSX behaviour where setsockopt returns EINVAL if the peer have already
        disconnected. Thus, nn_utcp_setsockopt() can succeed on OSX even though
        the option value was invalid, but the peer have already closed the
        connection. This behaviour should be relatively harmless. */
    rc = setsockopt (self->s, level, optname, optval, (socklen_t) optlen);
#if defined NN_HAVE_OSX
    if (rc != 0 && errno != EINVAL)
        return -errno;
#else
    if (rc != 0)
        return -errno;
#endif

    return 0;
}

int nn_utcp_bind (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;
    int opt;

    /*  The socket can be bound only before it's connected. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    /*  Allow re-using the address. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    errno_assert (rc == 0);

    rc = bind (self->s, addr, (socklen_t) addrlen);
    if (rc != 0)
        return -errno;

    return 0;
}

int nn_utcp_listen (struct nn_utcp *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    /*  Start listening for incoming connections. */
    rc = listen (self->s, backlog);
    if (rc != 0)
        return -errno;

    /*  Notify the state machine. */
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_LISTEN);

    return 0;
}

void nn_utcp_accept (struct nn_utcp *self, struct nn_utcp *listener)
{
    int s;

    /*  Start the actual accepting. */
    if (nn_fsm_isidle(&self->fsm)) {
        nn_fsm_start (&self->fsm);
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_BEING_ACCEPTED);
    }
    nn_fsm_action (&listener->fsm, NN_USOCK_ACTION_ACCEPT);

    /*  Try to accept new connection in synchronous manner. */
#if NN_HAVE_ACCEPT4
    s = accept4 (listener->s, NULL, NULL, SOCK_CLOEXEC);
#else
    s = accept (listener->s, NULL, NULL);
#endif

    /*  Immediate success. */
    if (s >= 0) {
        /*  Disassociate the listener socket from the accepted
            socket. Is useful if we restart accepting on ACCEPT_ERROR  */
        listener->asock = NULL;
        self->asock = NULL;

        nn_usock_init_from_fd (self, s);
        nn_fsm_action (&listener->fsm, NN_USOCK_ACTION_DONE);
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_DONE);
        return;
    }

    /*  Detect a failure. Note that in ECONNABORTED case we simply ignore
        the error and wait for next connection in asynchronous manner. */
    errno_assert (errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == ECONNABORTED || errno == ENFILE || errno == EMFILE ||
        errno == ENOBUFS || errno == ENOMEM);

    /*  Pair the two sockets.  They are already paired in case
        previous attempt failed on ACCEPT_ERROR  */
    nn_assert (!self->asock || self->asock == listener);
    self->asock = listener;
    nn_assert (!listener->asock || listener->asock == self);
    listener->asock = self;

    /*  Some errors are just ok to ignore for now.  We also stop repeating
        any errors until next IN_FD event so that we are not in a tight loop
        and allow processing other events in the meantime  */
    if (errno != EAGAIN && errno != EWOULDBLOCK
        && errno != ECONNABORTED && errno != listener->errnum)
    {
        listener->errnum = errno;
        listener->state = NN_USOCK_STATE_ACCEPTING_ERROR;
        nn_fsm_raise (&listener->fsm,
            &listener->event_error, NN_STREAM_ACCEPT_ERROR);
        return;
    }

    /*  Ask the worker thread to wait for the new connection. */
    nn_worker_execute (listener->worker, &listener->task_accept);
}

void nn_utcp_connect (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;

    /*  Notify the state machine that we've started connecting. */
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_CONNECT);

    /* Do the connect itself. */
    rc = connect (self->s, addr, (socklen_t) addrlen);

    /* Immediate success. */
    if (rc == 0) {
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_DONE);
        return;
    }

    /*  Immediate error. */
    if (errno != EINPROGRESS) {
        self->errnum = errno;
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
        return;
    }

    /*  Start asynchronous connect. */
    nn_worker_execute (self->worker, &self->task_connecting);
}

void nn_utcp_send (struct nn_utcp *self, const struct nn_iovec *iov,
    int iovcnt)
{
    int rc;
    int i;
    int out;

    /*  Make sure that the socket is actually alive. */
    if (self->state != NN_USOCK_STATE_ACTIVE) {
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
        return;
    }

    /*  Copy the iovecs to the socket. */
    nn_assert (iovcnt <= NN_UTCP_MAX_IOVCNT);
    self->out.hdr.msg_iov = self->out.iov;
    out = 0;
    for (i = 0; i != iovcnt; ++i) {
        if (iov [i].iov_len == 0)
            continue;
        self->out.iov [out].iov_base = iov [i].iov_base;
        self->out.iov [out].iov_len = iov [i].iov_len;
        out++;
    }
    self->out.hdr.msg_iovlen = out;

    /*  Try to send the data immediately. */
    rc = nn_usock_send_raw (self, &self->out.hdr);

    /*  Success. */
    if (rc == 0) {
        nn_fsm_raise (&self->fsm, &self->event_sent, NN_STREAM_SENT);
        return;
    }

    /*  Errors. */
    if (rc != -EAGAIN) {
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
        return;
    }

    /*  Ask the worker thread to send the remaining data. */
    nn_worker_execute (self->worker, &self->task_send);
}

void nn_utcp_recv (struct nn_utcp *self, void *buf, size_t len)
{
    int rc;
    size_t nbytes;

    /*  Make sure that the socket is actually alive. */
    if (self->state != NN_USOCK_STATE_ACTIVE) {
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
        return;
    }

    /*  Try to receive the data immediately. */
    nbytes = len;
    rc = nn_usock_recv_raw (self, buf, &nbytes);
    if (rc < 0) {
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ERROR);
        return;
    }

    /*  Success. */
    if (nbytes == len) {
        nn_fsm_raise (&self->fsm, &self->event_received, NN_STREAM_RECEIVED);
        return;
    }

    /*  There are still data to receive in the background. */
    self->in.buf = ((uint8_t*) buf) + nbytes;
    self->in.len = len - nbytes;

    /*  Ask the worker thread to receive the remaining data. */
    nn_worker_execute (self->worker, &self->task_recv);
}

static int nn_internal_tasks (struct nn_utcp *usock, int src, int type)
{

/******************************************************************************/
/*  Internal tasks sent from the user thread to the worker thread.            */
/******************************************************************************/
    switch (src) {
    case NN_USOCK_SRC_TASK_SEND:
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case NN_USOCK_SRC_TASK_RECV:
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    case NN_USOCK_SRC_TASK_CONNECTED:
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        return 1;
    case NN_USOCK_SRC_TASK_CONNECTING:
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        nn_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case NN_USOCK_SRC_TASK_ACCEPT:
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        nn_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    }

    return 0;
}

static void nn_usock_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_utcp *usock;

    usock = nn_cont (self, struct nn_utcp, fsm);

    if (nn_internal_tasks (usock, src, type))
        return;

    if (src == NN_FSM_ACTION && type == NN_FSM_STOP) {

        /*  Socket in ACCEPTING or CANCELLING state cannot be closed.
            Stop the socket being accepted first. */
        nn_assert (usock->state != NN_USOCK_STATE_ACCEPTING &&
            usock->state != NN_USOCK_STATE_CANCELLING);

        usock->errnum = 0;

        /*  Synchronous stop. */
        if (usock->state == NN_USOCK_STATE_IDLE)
            goto finish3;
        if (usock->state == NN_USOCK_STATE_DONE)
            goto finish2;
        if (usock->state == NN_USOCK_STATE_STARTING ||
            usock->state == NN_USOCK_STATE_ACCEPTED ||
            usock->state == NN_USOCK_STATE_ACCEPTING_ERROR ||
            usock->state == NN_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (usock->state == NN_USOCK_STATE_BEING_ACCEPTED) {
            nn_fsm_action (&usock->asock->fsm, NN_USOCK_ACTION_CANCEL);
            usock->state = NN_USOCK_STATE_STOPPING_ACCEPT;
            return;
        }

        /*  Asynchronous stop. */
        if (usock->state != NN_USOCK_STATE_REMOVING_FD)
            nn_usock_async_stop (usock);
        usock->state = NN_USOCK_STATE_STOPPING;
        return;
    }
    if (usock->state == NN_USOCK_STATE_STOPPING_ACCEPT) {
        nn_assert (src == NN_FSM_ACTION && type == NN_USOCK_ACTION_DONE);
        goto finish2;
    }
    if (usock->state == NN_USOCK_STATE_STOPPING) {
        if (src != NN_USOCK_SRC_TASK_STOP)
            return;
        nn_assert (type == NN_WORKER_TASK_EXECUTE);
        nn_worker_rm_fd (usock->worker, &usock->wfd);
    finish1:
        nn_closefd (usock->s);
        usock->s = -1;
    finish2:
        usock->state = NN_USOCK_STATE_IDLE;
        nn_fsm_stopped (&usock->fsm, NN_STREAM_STOPPED);
    finish3:
        return;
    }

    nn_fsm_bad_state(usock->state, src, type);
}

static void nn_utcp_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    int rc;
    struct nn_utcp *usock;
    int s;
    size_t sz;
    int sockerr;

    usock = nn_cont (self, struct nn_utcp, fsm);

    if(nn_internal_tasks(usock, src, type))
        return;

    switch (usock->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  nn_usock object is initialised, but underlying OS socket is not yet       */
/*  created.                                                                  */
/******************************************************************************/
    case NN_USOCK_STATE_IDLE:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                usock->state = NN_USOCK_STATE_STARTING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  STARTING state.                                                           */
/*  Underlying OS socket is created, but it's not yet passed to the worker    */
/*  thread. In this state we can set socket options, local and remote         */
/*  address etc.                                                              */
/******************************************************************************/
    case NN_USOCK_STATE_STARTING:

        /*  Events from the owner of the usock. */
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_LISTEN:
                usock->state = NN_USOCK_STATE_LISTENING;
                return;
            case NN_USOCK_ACTION_CONNECT:
                usock->state = NN_USOCK_STATE_CONNECTING;
                return;
            case NN_USOCK_ACTION_BEING_ACCEPTED:
                usock->state = NN_USOCK_STATE_BEING_ACCEPTED;
                return;
            case NN_USOCK_ACTION_STARTED:
                nn_worker_add_fd (usock->worker, usock->s, &usock->wfd);
                usock->state = NN_USOCK_STATE_ACTIVE;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  BEING_ACCEPTED state.                                                     */
/*  accept() was called on the usock. Now the socket is waiting for a new     */
/*  connection to arrive.                                                     */
/******************************************************************************/
    case NN_USOCK_STATE_BEING_ACCEPTED:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_ACCEPTED;
                nn_fsm_raise (&usock->fsm, &usock->event_established,
                    NN_USOCK_ACCEPTED);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTED state.                                                           */
/*  Connection was accepted, now it can be tuned. Afterwards, it'll move to   */
/*  the active state.                                                         */
/******************************************************************************/
    case NN_USOCK_STATE_ACCEPTED:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ACTIVATE:
                nn_worker_add_fd (usock->worker, usock->s, &usock->wfd);
                usock->state = NN_USOCK_STATE_ACTIVE;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Asynchronous connecting is going on.                                      */
/******************************************************************************/
    case NN_USOCK_STATE_CONNECTING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_ACTIVE;
                nn_worker_execute (usock->worker, &usock->task_connected);
                nn_fsm_raise (&usock->fsm, &usock->event_established,
                    NN_STREAM_CONNECTED);
                return;
            case NN_USOCK_ACTION_ERROR:
                nn_closefd (usock->s);
                usock->s = -1;
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error,
                    NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_FD:
            switch (type) {
            case NN_WORKER_FD_OUT:
                nn_worker_reset_out (usock->worker, &usock->wfd);
                usock->state = NN_USOCK_STATE_ACTIVE;
                sockerr = nn_usock_geterr(usock);
                if (sockerr == 0) {
                    nn_fsm_raise (&usock->fsm, &usock->event_established,
                        NN_STREAM_CONNECTED);
                } else {
                    usock->errnum = sockerr;
                    nn_worker_rm_fd (usock->worker, &usock->wfd);
                    rc = close (usock->s);
                    errno_assert (rc == 0);
                    usock->s = -1;
                    usock->state = NN_USOCK_STATE_DONE;
                    nn_fsm_raise (&usock->fsm,
                        &usock->event_error, NN_STREAM_ERROR);
                }
                return;
            case NN_WORKER_FD_ERR:
                nn_worker_rm_fd (usock->worker, &usock->wfd);
                nn_closefd (usock->s);
                usock->s = -1;
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Socket is connected. It can be used for sending and receiving data.       */
/******************************************************************************/
    case NN_USOCK_STATE_ACTIVE:
        switch (src) {
        case NN_USOCK_SRC_FD:
            switch (type) {
            case NN_WORKER_FD_IN:
                sz = usock->in.len;
                rc = nn_usock_recv_raw (usock, usock->in.buf, &sz);
                if (rc == 0) {
                    usock->in.len -= sz;
                    usock->in.buf += sz;
                    if (!usock->in.len) {
                        nn_worker_reset_in (usock->worker, &usock->wfd);
                        nn_fsm_raise (&usock->fsm, &usock->event_received,
                            NN_STREAM_RECEIVED);
                    }
                    return;
                }
                errnum_assert (rc == -ECONNRESET, -rc);
                goto error;
            case NN_WORKER_FD_OUT:
                rc = nn_usock_send_raw (usock, &usock->out.hdr);
                if (rc == 0) {
                    nn_worker_reset_out (usock->worker, &usock->wfd);
                    nn_fsm_raise (&usock->fsm, &usock->event_sent,
                        NN_STREAM_SENT);
                    return;
                }
                if (rc == -EAGAIN)
                    return;
                errnum_assert (rc == -ECONNRESET, -rc);
                goto error;
            case NN_WORKER_FD_ERR:
            error:
                nn_worker_rm_fd (usock->worker, &usock->wfd);
                nn_closefd (usock->s);
                usock->s = -1;
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ERROR:
                usock->state = NN_USOCK_STATE_REMOVING_FD;
                nn_usock_async_stop (usock);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source(usock->state, src, type);
        }

/******************************************************************************/
/*  REMOVING_FD state.                                                        */
/******************************************************************************/
    case NN_USOCK_STATE_REMOVING_FD:
        switch (src) {
        case NN_USOCK_SRC_TASK_STOP:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:
                nn_worker_rm_fd (usock->worker, &usock->wfd);
                nn_closefd (usock->s);
                usock->s = -1;
                usock->state = NN_USOCK_STATE_DONE;
                nn_fsm_raise (&usock->fsm, &usock->event_error, NN_STREAM_ERROR);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }

            /*  Events from the file descriptor are ignored while it is being
                removed. */
        case NN_USOCK_SRC_FD:
            return;

        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ERROR:
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  Socket is closed. The only thing that can be done in this state is        */
/*  stopping the usock.                                                       */
/******************************************************************************/
    case NN_USOCK_STATE_DONE:
        return;

/******************************************************************************/
/*  LISTENING state.                                                          */
/*  Socket is listening for new incoming connections, however, user is not    */
/*  accepting a new connection.                                               */
/******************************************************************************/
    case NN_USOCK_STATE_LISTENING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ACCEPT:
                usock->state = NN_USOCK_STATE_ACCEPTING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  User is waiting asynchronouslyfor a new inbound connection                */
/*  to be accepted.                                                           */
/******************************************************************************/
    case NN_USOCK_STATE_ACCEPTING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_DONE:
                usock->state = NN_USOCK_STATE_LISTENING;
                return;
            case NN_USOCK_ACTION_CANCEL:
                usock->state = NN_USOCK_STATE_CANCELLING;
                nn_worker_execute (usock->worker, &usock->task_stop);
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_FD:
            switch (type) {
            case NN_WORKER_FD_IN:

                /*  New connection arrived in asynchronous manner. */
#if NN_HAVE_ACCEPT4
                s = accept4 (usock->s, NULL, NULL, SOCK_CLOEXEC);
#else
                s = accept (usock->s, NULL, NULL);
#endif

                /*  ECONNABORTED is an valid error. New connection was closed
                    by the peer before we were able to accept it. If it happens
                    do nothing and wait for next incoming connection. */
                if (s < 0 && errno == ECONNABORTED)
                    return;

                /*  Resource allocation errors. It's not clear from POSIX
                    specification whether the new connection is closed in this
                    case or whether it remains in the backlog. In the latter
                    case it would be wise to wait here for a while to prevent
                    busy looping. */
                if (s < 0 && (errno == ENFILE || errno == EMFILE ||
                    errno == ENOBUFS || errno == ENOMEM)) {
                    usock->errnum = errno;
                    usock->state = NN_USOCK_STATE_ACCEPTING_ERROR;

                    /*  Wait till the user starts accepting once again. */
                    nn_worker_rm_fd (usock->worker, &usock->wfd);

                    nn_fsm_raise (&usock->fsm,
                        &usock->event_error, NN_STREAM_ACCEPT_ERROR);
                    return;
                }

                /* Any other error is unexpected. */
                errno_assert (s >= 0);

                /*  Initialise the new usock object. */
                nn_usock_init_from_fd (usock->asock, s);
                usock->asock->state = NN_USOCK_STATE_ACCEPTED;

                /*  Notify the user that connection was accepted. */
                nn_fsm_raise (&usock->asock->fsm,
                    &usock->asock->event_established, NN_USOCK_ACCEPTED);

                /*  Disassociate the listener socket from the accepted
                    socket. */
                usock->asock->asock = NULL;
                usock->asock = NULL;

                /*  Wait till the user starts accepting once again. */
                nn_worker_rm_fd (usock->worker, &usock->wfd);
                usock->state = NN_USOCK_STATE_LISTENING;

                return;

            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING_ERROR state.                                                    */
/*  Waiting the socket to accept the error and restart                        */
/******************************************************************************/
    case NN_USOCK_STATE_ACCEPTING_ERROR:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_USOCK_ACTION_ACCEPT:
                usock->state = NN_USOCK_STATE_ACCEPTING;
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  CANCELLING state.                                                         */
/******************************************************************************/
    case NN_USOCK_STATE_CANCELLING:
        switch (src) {
        case NN_USOCK_SRC_TASK_STOP:
            switch (type) {
            case NN_WORKER_TASK_EXECUTE:
                nn_worker_rm_fd (usock->worker, &usock->wfd);
                usock->state = NN_USOCK_STATE_LISTENING;

                /*  Notify the accepted socket that it was stopped. */
                nn_fsm_action (&usock->asock->fsm, NN_USOCK_ACTION_DONE);

                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        case NN_USOCK_SRC_FD:
            switch (type) {
            case NN_WORKER_FD_IN:
                return;
            default:
                nn_fsm_bad_action (usock->state, src, type);
            }
        default:
            nn_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  Invalid state                                                             */
/******************************************************************************/
    default:
        nn_fsm_bad_state (usock->state, src, type);
    }
}

static int nn_usock_send_raw (struct nn_utcp *self, struct msghdr *hdr)
{
    ssize_t nbytes;

    /*  Try to send the data. */
#if defined MSG_NOSIGNAL
    nbytes = sendmsg (self->s, hdr, MSG_NOSIGNAL);
#else
    nbytes = sendmsg (self->s, hdr, 0);
#endif

    /*  Handle errors. */
    if (nbytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            nbytes = 0;
        else {

            /*  If the connection fails, return ECONNRESET. */
            return -ECONNRESET;
        }
    }

    /*  Some bytes were sent. Adjust the iovecs accordingly. */
    while (nbytes) {
        if (nbytes >= (ssize_t)hdr->msg_iov->iov_len) {
            --hdr->msg_iovlen;
            if (!hdr->msg_iovlen) {
                nn_assert (nbytes == (ssize_t)hdr->msg_iov->iov_len);
                return 0;
            }
            nbytes -= hdr->msg_iov->iov_len;
            ++hdr->msg_iov;
        }
        else {
            *((uint8_t**) &(hdr->msg_iov->iov_base)) += nbytes;
            hdr->msg_iov->iov_len -= nbytes;
            return -EAGAIN;
        }
    }

    if (hdr->msg_iovlen > 0)
        return -EAGAIN;

    return 0;
}

static int nn_usock_recv_raw (struct nn_utcp *self, void *buf, size_t *len)
{
    size_t sz;
    size_t length;
    ssize_t nbytes;
    struct iovec iov;
    struct msghdr hdr;
    unsigned char ctrl [256];
#if defined NN_HAVE_MSG_CONTROL
    struct cmsghdr *cmsg;
#endif

    /*  If batch buffer doesn't exist, allocate it. The point of delayed
        deallocation to allow non-receiving sockets, such as TCP listening
        sockets, to do without the batch buffer. */
    if (!self->in.batch) {
        self->in.batch = nn_alloc (NN_USOCK_BATCH_SIZE, "AIO batch buffer");
        nn_assert_alloc (self->in.batch);
    }

    /*  Try to satisfy the recv request by data from the batch buffer. */
    length = *len;
    sz = self->in.batch_len - self->in.batch_pos;
    if (sz) {
        if (sz > length)
            sz = length;
        memcpy (buf, self->in.batch + self->in.batch_pos, sz);
        self->in.batch_pos += sz;
        buf = ((char*) buf) + sz;
        length -= sz;
        if (!length)
            return 0;
    }

    /*  If recv request is greater than the batch buffer, get the data directly
        into the place. Otherwise, read data to the batch buffer. */
    if (length > NN_USOCK_BATCH_SIZE) {
        iov.iov_base = buf;
        iov.iov_len = length;
    }
    else {
        iov.iov_base = self->in.batch;
        iov.iov_len = NN_USOCK_BATCH_SIZE;
    }
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
#if defined NN_HAVE_MSG_CONTROL
    hdr.msg_control = ctrl;
    hdr.msg_controllen = sizeof (ctrl);
#else
    *((int*) ctrl) = -1;
    hdr.msg_accrights = ctrl;
    hdr.msg_accrightslen = sizeof (int);
#endif
    nbytes = recvmsg (self->s, &hdr, 0);

    /*  Handle any possible errors. */
    if (nbytes <= 0) {

        if (nbytes == 0)
            return -ECONNRESET;

        /*  Zero bytes received. */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            nbytes = 0;
        else {

            /*  If the peer closes the connection, return ECONNRESET. */
            return -ECONNRESET;
        }
    }

    /*  Extract the associated file descriptor, if any. */
    if (nbytes > 0) {
#if defined NN_HAVE_MSG_CONTROL
        cmsg = CMSG_FIRSTHDR (&hdr);
        while (cmsg) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                if (self->in.pfd) {
                    *self->in.pfd = *((int*) CMSG_DATA (cmsg));
                    self->in.pfd = NULL;
                }
                else {
                    nn_closefd (*((int*) CMSG_DATA (cmsg)));
                }
                break;
            }
            cmsg = CMSG_NXTHDR (&hdr, cmsg);
        }
#else
        if (hdr.msg_accrightslen > 0) {
            nn_assert (hdr.msg_accrightslen == sizeof (int));
            if (self->in.pfd) {
                *self->in.pfd = *((int*) hdr.msg_accrights);
                self->in.pfd = NULL;
            }
            else {
                nn_closefd (*((int*) hdr.msg_accrights));
            }
        }
#endif
    }

    /*  If the data were received directly into the place we can return
        straight away. */
    if (length > NN_USOCK_BATCH_SIZE) {
        length -= nbytes;
        *len -= length;
        return 0;
    }

    /*  New data were read to the batch buffer. Copy the requested amount of it
        to the user-supplied buffer. */
    self->in.batch_len = nbytes;
    self->in.batch_pos = 0;
    if (nbytes) {
        sz = nbytes > (ssize_t)length ? length : (size_t)nbytes;
        memcpy (buf, self->in.batch, sz);
        length -= sz;
        self->in.batch_pos += sz;
    }

    *len -= length;
    return 0;
}

static int nn_usock_geterr (struct nn_utcp *self)
{
    int rc;
    int opt;
#if defined NN_HAVE_HPUX
    int optsz;
#else
    socklen_t optsz;
#endif

    opt = 0;
    optsz = sizeof (opt);
    rc = getsockopt (self->s, SOL_SOCKET, SO_ERROR, &opt, &optsz);

    /*  The following should handle both Solaris and UNIXes derived from BSD. */
    if (rc == -1)
        return errno;
    errno_assert (rc == 0);
    nn_assert (optsz == sizeof (opt));
    return opt;
}

#endif

void nn_utcp_activate (struct nn_utcp *self)
{
    nn_fsm_action (&self->fsm, NN_USOCK_ACTION_ACTIVATE);
}

int nn_utcp_isidle (struct nn_utcp *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_utcp_swap_owner (struct nn_utcp *self, struct nn_fsm_owner *owner)
{
    nn_fsm_swap_owner (&self->fsm, owner);
}

void nn_utcp_stop (struct nn_utcp *self)
{
    nn_fsm_stop (&self->fsm);
}
