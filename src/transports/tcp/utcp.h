/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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

#ifndef NN_UTCP_INCLUDED
#define NN_UTCP_INCLUDED

/*  Import the definition of nn_iovec. */
#include "../../nn.h"

#include "../../aio/fsm.h"
#include "../../aio/stream.h"
#include "../../aio/worker.h"

/*  Maximum number of iovecs that can be passed to nn_utcp_send function. */
#define NN_UTCP_MAX_IOVCNT 3

/*  Size of the buffer used for batch-reads of inbound data. To keep the
    performance optimal make sure that this value is larger than network MTU. */
#define NN_USOCK_BATCH_SIZE 2048

#if defined NN_HAVE_WINDOWS

#include "../../utils/win.h"

struct nn_utcp {

    /*  The state machine. */
    struct nn_fsm fsm;
    int state;

    /*  The actual underlying socket. May be cast to HANDLE for operations. */
    SOCKET s;

    /*  Asynchronous operations being executed on the socket. */
    struct nn_worker_op in;
    struct nn_worker_op out;

    /*  Size of pending I/O operations. Windows IOCP does not readily support
        TCP pushback without tracking pending requests at this layer, since
        operations such as WSASend and WSARecv will continue to happily return
        WSA_IO_PENDING rather than EAGAIN once the SNDBUF/RCVBUF is full. */
    size_t send_pending_sz;
    size_t recv_pending_sz;

    /*  Number of pending I/O operations. */
    size_t send_pending_count;
    size_t recv_pending_count;

    /*  When accepting new socket, they have to be created with same
        type as the listening socket. Thus, in listening socket we
        have to store its exact type. */
    int domain;
    int type;
    int protocol;

    /*  Events raised by the usock. */
    struct nn_fsm_event event_established;
    struct nn_fsm_event event_sent;
    struct nn_fsm_event event_received;
    struct nn_fsm_event event_error;

    /*  In ACCEPTING state points to the socket being accepted.
        In BEING_ACCEPTED state points to the listener socket. */
    struct nn_utcp *asock;

    /*  Buffer allocated for output of AcceptEx function. If accepting is not
        done on this socket, the field is set to NULL. */
    void *ainfo;

    /*  Errno remembered in NN_STREAM_ERROR state  */
    int errnum;
};

#else

#include "fsm.h"
#include "worker.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

struct nn_utcp {

    /*  State machine base class. */
    struct nn_fsm fsm;
    int state;

    /*  The worker thread the usock is associated with. */
    struct nn_worker *worker;

    /*  The underlying OS socket and handle that represents it in the poller. */
    int s;
    struct nn_worker_fd wfd;

    /*  Members related to receiving data. */
    struct {

        /*  The buffer being filled in at the moment. */
        uint8_t *buf;
        size_t len;

        /*  Buffer for batch-reading inbound data. */
        uint8_t *batch;

        /*  Size of the batch buffer. */
        size_t batch_len;

        /*  Current position in the batch buffer. The data preceding this
            position were already received by the user. The data that follow
            will be received in the future. */
        size_t batch_pos;

        /*  File descriptor received via SCM_RIGHTS, if any. */
        int *pfd;
    } in;

    /*  Members related to sending data. */
    struct {

        /*  msghdr being sent at the moment. */
        struct msghdr hdr;

        /*  List of buffers being sent at the moment. Referenced from 'hdr'. */
        struct iovec iov [NN_UTCP_MAX_IOVCNT];
    } out;

    /*  Asynchronous tasks for the worker. */
    struct nn_worker_task task_connecting;
    struct nn_worker_task task_connected;
    struct nn_worker_task task_accept;
    struct nn_worker_task task_send;
    struct nn_worker_task task_recv;
    struct nn_worker_task task_stop;

    /*  Events raised by the usock. */
    struct nn_fsm_event event_established;
    struct nn_fsm_event event_sent;
    struct nn_fsm_event event_received;
    struct nn_fsm_event event_error;

    /*  In ACCEPTING state points to the socket being accepted.
        In BEING_ACCEPTED state points to the listener socket. */
    struct nn_utcp *asock;

    /*  Errno remembered in NN_STREAM_ERROR state  */
    int errnum;
};
#endif

void nn_utcp_init (struct nn_utcp *self, int src, struct nn_fsm *owner);
void nn_utcp_term (struct nn_utcp *self);

int nn_utcp_isidle (struct nn_utcp *self);
int nn_utcp_start (struct nn_utcp *self, int domain, int type, int protocol);
void nn_utcp_stop (struct nn_utcp *self);

void nn_utcp_swap_owner (struct nn_utcp *self, struct nn_fsm_owner *owner);

int nn_utcp_setsockopt (struct nn_utcp *self, int level, int optname,
    const void *optval, size_t optlen);

int nn_utcp_bind (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen);
int nn_utcp_listen (struct nn_utcp *self, int backlog);

/*  Accept a new connection from a listener. When done, NN_USOCK_ACCEPTED
    event will be delivered to the accepted socket. To cancel the operation,
    stop the socket being accepted. Listening socket should not be stopped
    while accepting a new socket is underway. */
void nn_utcp_accept (struct nn_utcp *self, struct nn_utcp *listener);

/*  When all the tuning is done on the accepted socket, call this function
    to activate standard data transfer phase. */
void nn_utcp_activate (struct nn_utcp *self);

/*  Start connecting. Prior to this call the socket has to be bound to a local
    address. When connecting is done NN_STREAM_CONNECTED event will be reaised.
    If connecting fails NN_STREAM_ERROR event will be raised. */
void nn_utcp_connect (struct nn_utcp *self, const struct sockaddr *addr,
    size_t addrlen);

void nn_utcp_send (struct nn_utcp *self, const struct nn_iovec *iov,
    int iovcnt);
void nn_utcp_recv (struct nn_utcp *self, void *buf, size_t len);

#endif
