/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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

#include "../protocol.h"
#include "../transport.h"

#include "sock.h"
#include "global.h"
#include "ep.h"

#include "../utils/err.h"
#include "../utils/cont.h"
#include "../utils/clock.h"
#include "../utils/alloc.h"
#include "../utils/msg.h"

#include <limits.h>

/*  These bits specify whether individual efds are signalled or not at
    the moment. Storing this information allows us to avoid redundant signalling
    and unsignalling of the efd objects. */
#define NN_SOCK_FLAG_IN 1
#define NN_SOCK_FLAG_OUT 2

/*  Possible states of the socket. */
#define NN_SOCK_STATE_INIT 1
#define NN_SOCK_STATE_ACTIVE 2
#define NN_SOCK_STATE_STOPPING_EPS 3
#define NN_SOCK_STATE_STOPPING 4
#define NN_SOCK_STATE_FINI 5

/*  Events sent to the state machine. */
#define NN_SOCK_ACTION_STOPPED 1

/*  Subordinated source objects. */
#define NN_SOCK_SRC_EP 1

/*  Private functions. */
static struct nn_optset *nn_sock_optset (struct nn_sock *self, int id);
static int nn_sock_setopt_inner (struct nn_sock *self, int level,
    int option, const void *optval, size_t optvallen);
static void nn_sock_onleave (struct nn_ctx *self);
static void nn_sock_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);

/*  Initialize a socket.  A hold is placed on the initialized socket for
    the caller as well. */
int nn_sock_init (struct nn_sock *self, struct nn_socktype *socktype, int fd)
{
    int rc;
    int i;

    /* Make sure that at least one message direction is supported. */
    nn_assert (!(socktype->flags & NN_SOCKTYPE_FLAG_NOSEND) ||
        !(socktype->flags & NN_SOCKTYPE_FLAG_NORECV));

    /*  Create the AIO context for the SP socket. */
    nn_ctx_init (&self->ctx, nn_global_getpool (), nn_sock_onleave, 1);

    /*  Initialise the state machine. */
    nn_fsm_init_root (&self->fsm, nn_sock_handler,
        nn_sock_handler, &self->ctx);
    self->state = NN_SOCK_STATE_INIT;

    /*  Open the NN_SNDFD and NN_RCVFD efds. Do so, only if the socket type
        supports send/recv, as appropriate. */
    if (socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)
        memset (&self->sndfd, 0xcd, sizeof (self->sndfd));
    else {
        rc = nn_efd_init (&self->sndfd);
        if (rc < 0)
            return rc;
    }
    if (socktype->flags & NN_SOCKTYPE_FLAG_NORECV)
        memset (&self->rcvfd, 0xcd, sizeof (self->rcvfd));
    else {
        rc = nn_efd_init (&self->rcvfd);
        if (rc < 0) {
            if (!(socktype->flags & NN_SOCKTYPE_FLAG_NOSEND))
                nn_efd_term (&self->sndfd);
            return rc;
        }
    }
    nn_sem_init (&self->termsem);

    self->flags = 0;
    nn_list_init (&self->eps);
    nn_list_init (&self->sdeps);
    self->eid = 1;

    /*  Default values for NN_SOL_SOCKET options. */
    self->linger = 1000;
    self->sndbuf = 128 * 1024;
    self->rcvbuf = 128 * 1024;
    self->rcvmaxsize = 1024 * 1024;
    self->sndtimeo = -1;
    self->rcvtimeo = -1;
    self->reconnect_ivl = 100;
    self->reconnect_ivl_max = 0;
    self->maxttl = 8;
    self->ep_template.sndprio = 8;
    self->ep_template.rcvprio = 8;
    self->ep_template.ipv4only = 1;

    /* Initialize statistic entries */
    self->statistics.established_connections = 0;
    self->statistics.accepted_connections = 0;
    self->statistics.dropped_connections = 0;
    self->statistics.broken_connections = 0;
    self->statistics.connect_errors = 0;
    self->statistics.bind_errors = 0;
    self->statistics.accept_errors = 0;

    self->statistics.messages_sent = 0;
    self->statistics.messages_received = 0;
    self->statistics.bytes_sent = 0;
    self->statistics.bytes_received = 0;

    self->statistics.current_connections = 0;
    self->statistics.inprogress_connections = 0;
    self->statistics.current_snd_priority = 0;
    self->statistics.current_rcv_priority = 0;
    self->statistics.current_ep_errors = 0;

    /*  Should be pretty much enough space for just the number  */
    sprintf(self->socket_name, "%d", fd);

    /*  The transport-specific options are not initialised immediately,
        rather, they are allocated later on when needed. */
    for (i = 0; i != NN_MAX_TRANSPORT; ++i)
        self->optsets [i] = NULL;

    /*  Create the specific socket type itself. */
    rc = socktype->create ((void*) self, &self->sockbase);
    errnum_assert (rc == 0, -rc);
    self->socktype = socktype;

    /*  Launch the state machine. */
    nn_ctx_enter (&self->ctx);
    nn_fsm_start (&self->fsm);
    nn_ctx_leave (&self->ctx);

    return 0;
}

void nn_sock_stopped (struct nn_sock *self)
{
    /*  TODO: Do the following in a more sane way. */
    self->fsm.stopped.dest = &self->fsm;
    self->fsm.stopped.src = NN_FSM_ACTION;
    self->fsm.stopped.srcptr = NULL;
    self->fsm.stopped.type = NN_SOCK_ACTION_STOPPED;
    nn_ctx_raise (self->fsm.ctx, &self->fsm.stopped);
}

/*  Stop the socket.  This will prevent new calls from aquiring a
    hold on the socket, cause endpoints to shut down, and wake any
    threads waiting to recv or send data. */
void nn_sock_stop (struct nn_sock *self)
{
    nn_ctx_enter (&self->ctx);
    nn_fsm_stop (&self->fsm);
    nn_ctx_leave (&self->ctx);
}

int nn_sock_term (struct nn_sock *self)
{
    int i;

    /*  NOTE: nn_sock_stop must have already been called. */

    /*  Some endpoints may still be active.  Here we are going to wait
        till they are all closed.  This loop is not interruptible, because
        making it so would leave a partially cleaned up socket, and we don't
        have a way to defer resource deallocation. */
    nn_sem_wait (&self->termsem);

    /*  Also, wait for all holds on the socket to be released.  */
    nn_ctx_wait_til_released (&self->ctx);

    /*  Threads that posted the semaphore(s) can still have the ctx locked
        for a short while processing their final events. Waiting to acquire the
        lock here one last time ensures the socket has fully completed its
        shutdown sequence. */

    /*  We are now certain that no other threads reference resources held by
        the socket. */
    nn_fsm_stopped_noevent (&self->fsm);
    nn_fsm_term (&self->fsm);
    nn_sem_term (&self->termsem);
    nn_list_term (&self->sdeps);
    nn_list_term (&self->eps);
    nn_ctx_term (&self->ctx);

    /*  Destroy any optsets associated with the socket. */
    for (i = 0; i != NN_MAX_TRANSPORT; ++i)
        if (self->optsets [i])
            self->optsets [i]->vfptr->destroy (self->optsets [i]);

    return 0;
}

struct nn_ctx *nn_sock_getctx (struct nn_sock *self)
{
    return &self->ctx;
}

int nn_sock_ispeer (struct nn_sock *self, int socktype)
{
    /*  If the peer implements a different SP protocol it is not a valid peer.
        Checking it here ensures that even if faulty protocol implementation
        allows for cross-protocol communication, it will never happen
        in practice. */
    if ((self->socktype->protocol & 0xfff0) != (socktype  & 0xfff0))
        return 0;

    /*  As long as the peer speaks the same protocol, socket type itself
        decides which socket types are to be accepted. */
    return self->socktype->ispeer (socktype);
}

int nn_sock_setopt (struct nn_sock *self, int level, int option,
    const void *optval, size_t optvallen)
{
    int rc;

    nn_ctx_enter (&self->ctx);
    rc = nn_sock_setopt_inner (self, level, option, optval, optvallen);
    nn_ctx_leave (&self->ctx);

    return rc;
}

static int nn_sock_setopt_inner (struct nn_sock *self, int level,
    int option, const void *optval, size_t optvallen)
{
    struct nn_optset *optset;
    int val;

    /*  Protocol-specific socket options. */
    if (level > NN_SOL_SOCKET)
        return self->sockbase->vfptr->setopt (self->sockbase, level, option,
            optval, optvallen);

    /*  Transport-specific options. */
    if (level < NN_SOL_SOCKET) {
        optset = nn_sock_optset (self, level);
        if (!optset)
            return -ENOPROTOOPT;
        return optset->vfptr->setopt (optset, option, optval, optvallen);
    }

    nn_assert (level == NN_SOL_SOCKET);

    /*  Special-casing socket name for now as it's the only string option  */
    if (option == NN_SOCKET_NAME) {
        if (optvallen > 63)
            return -EINVAL;
        memcpy (self->socket_name, optval, optvallen);
        self->socket_name [optvallen] = 0;
        return 0;
    }

    /*  At this point we assume that all options are of type int. */
    if (optvallen != sizeof (int))
        return -EINVAL;
    val = *(int*) optval;

    /*  Generic socket-level options. */
    switch (option) {
    case NN_LINGER:
        self->linger = val;
        return 0;
    case NN_SNDBUF:
        if (val < 0)
            return -EINVAL;
        self->sndbuf = val;
        return 0;
    case NN_RCVBUF:
        if (val < 0)
            return -EINVAL;
        self->rcvbuf = val;
        return 0;
    case NN_RCVMAXSIZE:
        if (val < -1)
            return -EINVAL;
        self->rcvmaxsize = val;
        return 0;
    case NN_SNDTIMEO:
        self->sndtimeo = val;
        return 0;
    case NN_RCVTIMEO:
        self->rcvtimeo = val;
        return 0;
    case NN_RECONNECT_IVL:
        if (val < 0)
            return -EINVAL;
        self->reconnect_ivl = val;
        return 0;
    case NN_RECONNECT_IVL_MAX:
        if (val < 0)
            return -EINVAL;
        self->reconnect_ivl_max = val;
        return 0;
    case NN_SNDPRIO:
        if (val < 1 || val > 16)
            return -EINVAL;
        self->ep_template.sndprio = val;
        return 0;
    case NN_RCVPRIO:
        if (val < 1 || val > 16)
            return -EINVAL;
        self->ep_template.rcvprio = val;
        return 0;
    case NN_IPV4ONLY:
        if (val != 0 && val != 1)
            return -EINVAL;
        self->ep_template.ipv4only = val;
        return 0;
    case NN_MAXTTL:
        if (val < 1 || val > 255)
            return -EINVAL;
        self->maxttl = val;
        return 0;
    }

    return -ENOPROTOOPT;
}

int nn_sock_getopt (struct nn_sock *self, int level, int option,
    void *optval, size_t *optvallen)
{
    int rc;

    nn_ctx_enter (&self->ctx);
    rc = nn_sock_getopt_inner (self, level, option, optval, optvallen);
    nn_ctx_leave (&self->ctx);

    return rc;
}

int nn_sock_getopt_inner (struct nn_sock *self, int level,
    int option, void *optval, size_t *optvallen)
{
    int rc;
    struct nn_optset *optset;
    int intval;
    nn_fd fd;

    /*  Protocol-specific socket options. */
    if (level > NN_SOL_SOCKET)
        return rc = self->sockbase->vfptr->getopt (self->sockbase,
            level, option, optval, optvallen);

    /*  Transport-specific options. */
    if (level < NN_SOL_SOCKET) {
        optset = nn_sock_optset (self, level);
        if (!optset)
            return -ENOPROTOOPT;
        return optset->vfptr->getopt (optset, option, optval, optvallen);
    }

    nn_assert (level == NN_SOL_SOCKET);

    /*  Generic socket-level options. */
    switch (option) {
    case NN_DOMAIN:
        intval = self->socktype->domain;
        break;
    case NN_PROTOCOL:
        intval = self->socktype->protocol;
        break;
    case NN_LINGER:
        intval = self->linger;
        break;
    case NN_SNDBUF:
        intval = self->sndbuf;
        break;
    case NN_RCVBUF:
        intval = self->rcvbuf;
        break;
    case NN_RCVMAXSIZE:
        intval = self->rcvmaxsize;
        break;
    case NN_SNDTIMEO:
        intval = self->sndtimeo;
        break;
    case NN_RCVTIMEO:
        intval = self->rcvtimeo;
        break;
    case NN_RECONNECT_IVL:
        intval = self->reconnect_ivl;
        break;
    case NN_RECONNECT_IVL_MAX:
        intval = self->reconnect_ivl_max;
        break;
    case NN_SNDPRIO:
        intval = self->ep_template.sndprio;
        break;
    case NN_RCVPRIO:
        intval = self->ep_template.rcvprio;
        break;
    case NN_IPV4ONLY:
        intval = self->ep_template.ipv4only;
        break;
    case NN_MAXTTL:
        intval = self->maxttl;
        break;
    case NN_SNDFD:
        if (self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)
            return -ENOPROTOOPT;
        fd = nn_efd_getfd (&self->sndfd);
        memcpy (optval, &fd,
            *optvallen < sizeof (nn_fd) ? *optvallen : sizeof (nn_fd));
        *optvallen = sizeof (nn_fd);
        return 0;
    case NN_RCVFD:
        if (self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)
            return -ENOPROTOOPT;
        fd = nn_efd_getfd (&self->rcvfd);
        memcpy (optval, &fd,
            *optvallen < sizeof (nn_fd) ? *optvallen : sizeof (nn_fd));
        *optvallen = sizeof (nn_fd);
        return 0;
    case NN_SOCKET_NAME:
        strncpy (optval, self->socket_name, *optvallen);
        *optvallen = strlen(self->socket_name);
        return 0;
    default:
        return -ENOPROTOOPT;
    }

    memcpy (optval, &intval,
        *optvallen < sizeof (int) ? *optvallen : sizeof (int));
    *optvallen = sizeof (int);

    return 0;
}

int nn_sock_add_ep (struct nn_sock *self, struct nn_transport *transport,
    int bind, const char *addr)
{
    int rc;
    struct nn_ep *ep;
    int eid;

    nn_ctx_enter (&self->ctx);

    /*  Instantiate the endpoint. */
    ep = nn_alloc (sizeof (struct nn_ep), "endpoint");
    if (ep == NULL) {
        nn_ctx_leave (&self->ctx);
        return -ENOMEM;
    }
    rc = nn_ep_init (ep, NN_SOCK_SRC_EP, self, self->eid, transport,
        bind, addr);
    if (rc < 0) {
        nn_free (ep);
        nn_ctx_leave (&self->ctx);
        return rc;
    }
    nn_ep_start (ep);

    /*  Increase the endpoint ID for the next endpoint. */
    eid = self->eid;
    ++self->eid;

    /*  Add it to the list of active endpoints. */
    nn_list_insert (&self->eps, &ep->item, nn_list_end (&self->eps));

    nn_ctx_leave (&self->ctx);

    return eid;
}

int nn_sock_rm_ep (struct nn_sock *self, int eid)
{
    struct nn_list_item *it;
    struct nn_ep *ep;

    nn_ctx_enter (&self->ctx);

    /*  Find the specified enpoint. */
    ep = NULL;
    for (it = nn_list_begin (&self->eps);
          it != nn_list_end (&self->eps);
          it = nn_list_next (&self->eps, it)) {
        ep = nn_cont (it, struct nn_ep, item);
        if (ep->eid == eid)
            break;
        ep = NULL;
    }

    /*  The endpoint doesn't exist. */
    if (!ep) {
        nn_ctx_leave (&self->ctx);
        return -EINVAL;
    }

    /*  Move the endpoint from the list of active endpoints to the list
        of shutting down endpoints. */
    nn_list_erase (&self->eps, &ep->item);
    nn_list_insert (&self->sdeps, &ep->item, nn_list_end (&self->sdeps));

    /*  Ask the endpoint to stop. Actual terminatation may be delayed
        by the transport. */
    nn_ep_stop (ep);

    nn_ctx_leave (&self->ctx);

    return 0;
}

int nn_sock_send (struct nn_sock *self, struct nn_msg *msg, int flags)
{
    int rc;
    uint64_t deadline;
    uint64_t now;
    int timeout;

    /*  Some sockets types cannot be used for sending messages. */
    if (self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)
        return -ENOTSUP;

    nn_ctx_enter (&self->ctx);

    /*  Compute the deadline for SNDTIMEO timer. */
    if (self->sndtimeo < 0) {
        deadline = -1;
        timeout = -1;
    }
    else {
        deadline = nn_clock_ms() + self->sndtimeo;
        timeout = self->sndtimeo;
    }

    while (1) {

        switch (self->state) {
        case NN_SOCK_STATE_ACTIVE:
        case NN_SOCK_STATE_INIT:
             break;

        case NN_SOCK_STATE_STOPPING_EPS:
        case NN_SOCK_STATE_STOPPING:
        case NN_SOCK_STATE_FINI:
            /*  Socket closed or closing.  Should we return something
                else here; recvmsg(2) for example returns no data in
                this case, like read(2).  The use of indexed file
                descriptors is further problematic, as an FD can be reused
                leading to situations where technically the outstanding
                operation should refer to some other socket entirely.  */
            nn_ctx_leave (&self->ctx);
            return -EBADF;
        }

        /*  Try to send the message in a non-blocking way. */
        rc = self->sockbase->vfptr->send (self->sockbase, msg);
        if (rc == 0) {
            nn_ctx_leave (&self->ctx);
            return 0;
        }
        nn_assert (rc < 0);

        /*  Any unexpected error is forwarded to the caller. */
        if (rc != -EAGAIN) {
            nn_ctx_leave (&self->ctx);
            return rc;
        }

        /*  If the message cannot be sent at the moment and the send call
            is non-blocking, return immediately. */
        if (flags & NN_DONTWAIT) {
            nn_ctx_leave (&self->ctx);
            return -EAGAIN;
        }

        /*  With blocking send, wait while there are new pipes available
            for sending. */
        nn_ctx_leave (&self->ctx);
        rc = nn_efd_wait (&self->sndfd, timeout);
        if (rc == -ETIMEDOUT)
            return -ETIMEDOUT;
        if (rc == -EINTR)
            return -EINTR;
        if (rc == -EBADF)
            return -EBADF;
        errnum_assert (rc == 0, rc);
        nn_ctx_enter (&self->ctx);
        /*
         *  Double check if pipes are still available for sending
         */
        if (!nn_efd_wait (&self->sndfd, 0)) {
            self->flags |= NN_SOCK_FLAG_OUT;
        }

        /*  If needed, re-compute the timeout to reflect the time that have
            already elapsed. */
        if (self->sndtimeo >= 0) {
            now = nn_clock_ms();
            timeout = (int) (now > deadline ? 0 : deadline - now);
        }
    }
}

int nn_sock_recv (struct nn_sock *self, struct nn_msg *msg, int flags)
{
    int rc;
    int tries;
    uint64_t deadline;
    uint64_t now;
    int timeout;

    /*  Some sockets types cannot be used for receiving messages. */
    if (self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)
        return -ENOTSUP;

    nn_ctx_enter (&self->ctx);

    /*  Compute the deadline for RCVTIMEO timer. */
    if (self->rcvtimeo < 0) {
        deadline = -1;
        timeout = -1;
    }
    else {
        deadline = nn_clock_ms() + self->rcvtimeo;
        timeout = self->rcvtimeo;
    }
    tries = 0;
    while (1) {

        switch (self->state) {
        case NN_SOCK_STATE_ACTIVE:
        case NN_SOCK_STATE_INIT:
             break;

        case NN_SOCK_STATE_STOPPING_EPS:
        case NN_SOCK_STATE_STOPPING:
        case NN_SOCK_STATE_FINI:
            /*  Socket closed or closing.  Should we return something
                else here; recvmsg(2) for example returns no data in
                this case, like read(2).  The use of indexed file
                descriptors is further problematic, as an FD can be reused
                leading to situations where technically the outstanding
                operation should refer to some other socket entirely.  */
            nn_ctx_leave (&self->ctx);
            return -EBADF;
        }

        /*  Try to receive the message in a non-blocking way. */
        rc = self->sockbase->vfptr->recv (self->sockbase, msg);
        if (rc == 0) {
            nn_ctx_leave (&self->ctx);
            return 0;
        }
        nn_assert (rc < 0);

        /*  Any unexpected error is forwarded to the caller. */
        if (rc != -EAGAIN) {
            nn_ctx_leave (&self->ctx);
            return rc;
        }

        /*  If the message cannot be received at the moment and the recv call
            is non-blocking, return immediately. */
        if (flags & NN_DONTWAIT) {
            nn_ctx_leave (&self->ctx);
            return -EAGAIN;
        }

        /*  With blocking recv, wait while there are new pipes available
            for receiving. */
        nn_ctx_leave (&self->ctx);
        rc = nn_efd_wait (&self->rcvfd, timeout);
        if (rc == -ETIMEDOUT)
            return -ETIMEDOUT;
        if (rc == -EINTR)
            return -EINTR;
        if (rc == -EBADF)
            return -EBADF;
        errnum_assert (rc == 0, rc);
        nn_ctx_enter (&self->ctx);
        tries++;
        
        /*  Double check if pipes are still available for receiving. */
        if (!nn_efd_wait (&self->rcvfd, 0)) {
            self->flags |= NN_SOCK_FLAG_IN;
        }

        /*  If needed, re-compute the timeout to reflect the time that have
            already elapsed. */
        if (self->rcvtimeo >= 0) {
            now = nn_clock_ms();
            timeout = (int) (now > deadline ? 0 : deadline - now);
        }
    }
}

int nn_sock_add (struct nn_sock *self, struct nn_pipe *pipe)
{
    int rc;

    rc = self->sockbase->vfptr->add (self->sockbase, pipe);
    if (rc >= 0) {
        nn_sock_stat_increment (self, NN_STAT_CURRENT_CONNECTIONS, 1);
    }
    return rc;
}

void nn_sock_rm (struct nn_sock *self, struct nn_pipe *pipe)
{
    self->sockbase->vfptr->rm (self->sockbase, pipe);
    nn_sock_stat_increment (self, NN_STAT_CURRENT_CONNECTIONS, -1);
}

static void nn_sock_onleave (struct nn_ctx *self)
{
    struct nn_sock *sock;
    int events;

    sock = nn_cont (self, struct nn_sock, ctx);

    /*  Return early if no endpoints remain to be signalled. */
    if (sock->state == NN_SOCK_STATE_FINI || sock->state == NN_SOCK_STATE_STOPPING) {
        nn_assert (nn_list_empty (&sock->eps));
        nn_assert (nn_list_empty (&sock->sdeps));
        return;
    }

    nn_assert (sock->state == NN_SOCK_STATE_ACTIVE || sock->state == NN_SOCK_STATE_STOPPING_EPS);

    /*  Check whether socket is readable and/or writable at the moment. */
    events = sock->sockbase->vfptr->events (sock->sockbase);
    errnum_assert (events >= 0, -events);

    /*  Signal/unsignal IN as needed. */
    if (!(sock->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)) {
        if (events & NN_SOCKBASE_EVENT_IN) {
            if (!(sock->flags & NN_SOCK_FLAG_IN)) {
                sock->flags |= NN_SOCK_FLAG_IN;
                nn_efd_signal (&sock->rcvfd);
            }
        }
        else {
            if (sock->flags & NN_SOCK_FLAG_IN) {
                sock->flags &= ~NN_SOCK_FLAG_IN;
                nn_efd_unsignal (&sock->rcvfd);
            }
        }
    }

    /*  Signal/unsignal OUT as needed. */
    if (!(sock->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)) {
        if (events & NN_SOCKBASE_EVENT_OUT) {
            if (!(sock->flags & NN_SOCK_FLAG_OUT)) {
                sock->flags |= NN_SOCK_FLAG_OUT;
                nn_efd_signal (&sock->sndfd);
            }
        }
        else {
            if (sock->flags & NN_SOCK_FLAG_OUT) {
                sock->flags &= ~NN_SOCK_FLAG_OUT;
                nn_efd_unsignal (&sock->sndfd);
            }
        }
    }
}

static struct nn_optset *nn_sock_optset (struct nn_sock *self, int id)
{
    int index;
    struct nn_transport *tp;

    /*  Transport IDs are negative and start from -1. */
    index = (-id) - 1;

    /*  Check for invalid indices. */
    if (index < 0 || index >= NN_MAX_TRANSPORT)
        return NULL;

    /*  If the option set already exists return it. */
    if (self->optsets [index] != NULL)
        return self->optsets [index];

    /*  If the option set doesn't exist yet, create it. */
    tp = nn_global_transport (id);
    if (!tp)
        return NULL;
    if (!tp->optset)
        return NULL;
    self->optsets [index] = tp->optset ();

    return self->optsets [index];
}

void nn_sock_report_error (struct nn_sock *self, struct nn_ep *ep, int errnum)
{
    if (!nn_global_print_errors())
        return;

    if (errnum == 0)
        return;

    if (ep) {
        fprintf(stderr, "nanomsg: socket.%s[%s]: Error: %s\n",
            self->socket_name, nn_ep_getaddr(ep), nn_strerror(errnum));
    } else {
        fprintf(stderr, "nanomsg: socket.%s: Error: %s\n",
            self->socket_name, nn_strerror(errnum));
    }
}

void nn_sock_stat_increment (struct nn_sock *self, int name, int64_t increment)
{
    switch (name) {
    case NN_STAT_ESTABLISHED_CONNECTIONS:
        nn_assert (increment > 0);
        self->statistics.established_connections += increment;
        break;
    case NN_STAT_ACCEPTED_CONNECTIONS:
        nn_assert (increment > 0);
        self->statistics.accepted_connections += increment;
        break;
    case NN_STAT_DROPPED_CONNECTIONS:
        nn_assert (increment > 0);
        self->statistics.dropped_connections += increment;
        break;
    case NN_STAT_BROKEN_CONNECTIONS:
        nn_assert (increment > 0);
        self->statistics.broken_connections += increment;
        break;
    case NN_STAT_CONNECT_ERRORS:
        nn_assert (increment > 0);
        self->statistics.connect_errors += increment;
        break;
    case NN_STAT_BIND_ERRORS:
        nn_assert (increment > 0);
        self->statistics.bind_errors += increment;
        break;
    case NN_STAT_ACCEPT_ERRORS:
        nn_assert (increment > 0);
        self->statistics.accept_errors += increment;
        break;
    case NN_STAT_MESSAGES_SENT:
        nn_assert (increment > 0);
        self->statistics.messages_sent += increment;
        break;
    case NN_STAT_MESSAGES_RECEIVED:
        nn_assert (increment > 0);
        self->statistics.messages_received += increment;
        break;
    case NN_STAT_BYTES_SENT:
        nn_assert (increment >= 0);
        self->statistics.bytes_sent += increment;
        break;
    case NN_STAT_BYTES_RECEIVED:
        nn_assert (increment >= 0);
        self->statistics.bytes_received += increment;
        break;

    case NN_STAT_CURRENT_CONNECTIONS:
        nn_assert (increment > 0 ||
            self->statistics.current_connections >= -increment);
        nn_assert(increment < INT_MAX && increment > -INT_MAX);
        self->statistics.current_connections += (int) increment;
        break;
    case NN_STAT_INPROGRESS_CONNECTIONS:
        nn_assert (increment > 0 ||
            self->statistics.inprogress_connections >= -increment);
        nn_assert(increment < INT_MAX && increment > -INT_MAX);
        self->statistics.inprogress_connections += (int) increment;
        break;
    case NN_STAT_CURRENT_EP_ERRORS:
        nn_assert (increment > 0 ||
            self->statistics.current_ep_errors >= -increment);
        nn_assert(increment < INT_MAX && increment > -INT_MAX);
        self->statistics.current_ep_errors += (int) increment;
        break;
    default:
        nn_assert_unreachable ("Unexpected statistic name.");
        break;
    }
}

void nn_sock_statistic_set (struct nn_sock *self, int name, int value)
{
    switch (name) {
    case NN_STAT_CURRENT_SND_PRIORITY:
        nn_assert ((value > 0 && value <= 16) || value == -1);
        self->statistics.current_snd_priority = value;
        break;
    case NN_STAT_CURRENT_RCV_PRIORITY:
        nn_assert ((value > 0 && value <= 16) || value == -1);
        self->statistics.current_rcv_priority = value;
        break;
    default:
        nn_assert_unreachable ("Unexpected statistic name.");
        break;
    }
}

int nn_sock_hold (struct nn_sock *self)
{
    int rc;

    switch (self->state) {
    case NN_SOCK_STATE_ACTIVE:
    case NN_SOCK_STATE_INIT:
        nn_ctx_enter (&self->ctx);
        rc = nn_ctx_hold (&self->ctx);
        nn_ctx_leave (&self->ctx);
        break;
    case NN_SOCK_STATE_STOPPING:
    case NN_SOCK_STATE_STOPPING_EPS:
    case NN_SOCK_STATE_FINI:
        rc = -EBADF;
        break;
    default:
        nn_assert_unreachable ("Unexpected socket state.");
        break;
    }

    return rc;
}

void nn_sock_release (struct nn_sock *self)
{
    nn_ctx_release (&self->ctx);
}

static void nn_sock_handler (struct nn_fsm *myfsm, int src, int type,
    void *srcptr)
{
    struct nn_list_item *it;
    struct nn_sock *self;
    struct nn_ep *ep;

    self = nn_cont (myfsm, struct nn_sock, fsm);

    NN_FSM_JOB (NN_SOCK_STATE_INIT, NN_FSM_ACTION, NN_FSM_START) {
        self->state = NN_SOCK_STATE_ACTIVE;
        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_ACTIVE, NN_FSM_ACTION, NN_FSM_STOP) {
        /*  Close sndfd and rcvfd. This should make any current
            select/poll using SNDFD and/or RCVFD exit. */
        if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)) {
            nn_efd_stop (&self->rcvfd);
        }
        if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)) {
            nn_efd_stop (&self->sndfd);
        }

        /*  Ask all the associated endpoints to stop. */
        it = nn_list_begin (&self->eps);
        while (it != nn_list_end (&self->eps)) {
            ep = nn_cont (it, struct nn_ep, item);
            it = nn_list_next (&self->eps, it);
            nn_list_erase (&self->eps, &ep->item);
            nn_list_insert (&self->sdeps, &ep->item,
                nn_list_end (&self->sdeps));
            nn_ep_stop (ep);
        }


        /*  If all the endpoints are deallocated, we can start stopping
            protocol-specific part of the socket. If there' no stop function
            we can consider it stopped straight away. */
        if (!nn_list_empty (&self->sdeps)) {
            self->state = NN_SOCK_STATE_STOPPING_EPS;
            return;
        }

        nn_assert (nn_list_empty (&self->eps));
        if (!self->sockbase->vfptr->stop) {
            /*  Protocol-specific part of the socket is stopped.
                We can safely deallocate it. */
            self->sockbase->vfptr->destroy (self->sockbase);
            self->state = NN_SOCK_STATE_FINI;

            /*  Close the event FDs entirely. */
            if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)) {
                nn_efd_term (&self->rcvfd);
            }
            if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)) {
                nn_efd_term (&self->sndfd);
            }

            /*  Now we can unblock the application thread blocked in
                the nn_close() call. */
            nn_sem_post (&self->termsem);

            return;

        }
        self->state = NN_SOCK_STATE_STOPPING;
        self->sockbase->vfptr->stop (self->sockbase);
        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_STOPPING_EPS, NN_SOCK_SRC_EP, NN_EP_STOPPED) {
        /*  Endpoint is stopped. Now we can safely deallocate it. */
        ep = (struct nn_ep*) srcptr;
        nn_list_erase (&self->sdeps, &ep->item);
        nn_ep_term (ep);
        nn_free (ep);

        /*  If all the endpoints are deallocated, we can start stopping
            protocol-specific part of the socket. If there' no stop function
            we can consider it stopped straight away. */
        if (!nn_list_empty (&self->sdeps))
            return;
        nn_assert (nn_list_empty (&self->eps));
        if (!self->sockbase->vfptr->stop) {
            /*  Protocol-specific part of the socket is stopped.
                We can safely deallocate it. */
            self->sockbase->vfptr->destroy (self->sockbase);
            self->state = NN_SOCK_STATE_FINI;

            /*  Close the event FDs entirely. */
            if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)) {
                nn_efd_term (&self->rcvfd);
            }
            if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)) {
                nn_efd_term (&self->sndfd);
            }

            /*  Now we can unblock the application thread blocked in
                the nn_close() call. */
            nn_sem_post (&self->termsem);

            return;
        }
        self->state = NN_SOCK_STATE_STOPPING;
        self->sockbase->vfptr->stop (self->sockbase);
        return;

    }

    /*  We get here when the deallocation of the socket was delayed by the
        specific socket type. */
    NN_FSM_JOB (NN_SOCK_STATE_STOPPING, NN_FSM_ACTION, NN_SOCK_ACTION_STOPPED) {

        /*  Protocol-specific part of the socket is stopped.
            We can safely deallocate it. */
        self->sockbase->vfptr->destroy (self->sockbase);
        self->state = NN_SOCK_STATE_FINI;

        /*  Close the event FDs entirely. */
        if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NORECV)) {
            nn_efd_term (&self->rcvfd);
        }
        if (!(self->socktype->flags & NN_SOCKTYPE_FLAG_NOSEND)) {
            nn_efd_term (&self->sndfd);
        }

        /*  Now we can unblock the application thread blocked in
        the nn_close() call. */
        nn_sem_post (&self->termsem);

        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_ACTIVE, NN_SOCK_SRC_EP, NN_EP_STOPPED) {
        /*  This happens when an endpoint is closed using nn_shutdown() function. */
        ep = (struct nn_ep*) srcptr;
        nn_list_erase (&self->sdeps, &ep->item);
        nn_ep_term (ep);
        nn_free (ep);
        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_ACTIVE, NN_PIPE_SRC, NN_PIPE_IN) {
        self->sockbase->vfptr->in (self->sockbase, (struct nn_pipe*) srcptr);
        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_ACTIVE, NN_PIPE_SRC, NN_PIPE_OUT) {
        self->sockbase->vfptr->out (self->sockbase, (struct nn_pipe*) srcptr);
        return;
    }




    ////////////////////experimental
    NN_FSM_JOB (NN_SOCK_STATE_STOPPING_EPS, NN_PIPE_SRC, NN_PIPE_IN) {

        //nn_assert_unreachable ("JRD - does this always interrupt inproc deadlock on shutdown?");

        self->sockbase->vfptr->in (self->sockbase, (struct nn_pipe*) srcptr);
        return;
    }

    NN_FSM_JOB (NN_SOCK_STATE_STOPPING_EPS, NN_PIPE_SRC, NN_PIPE_OUT) {

        //nn_assert_unreachable ("JRD - does this always interrupt inproc deadlock on shutdown?");

        self->sockbase->vfptr->out (self->sockbase, (struct nn_pipe*) srcptr);
        return;
    }
    ////////////////////experimental
    
    nn_fsm_bad_state (self->state, src, type);
}
