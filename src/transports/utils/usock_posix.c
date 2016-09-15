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

//#if !defined NN_USE_WINSOCK

#include "../stream/ustream.h"

#include "../../utils/alloc.h"
#include "../../utils/closefd.h"
#include "../../utils/cont.h"
#include "../../utils/err.h"
#include "../../utils/attr.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

/*  Size of the buffer used for batch-reads of inbound data. To keep the
    performance optimal make sure that this value is larger than network MTU. */
#define NN_USOCK_POSIX_BATCH_SIZE 2048

struct nn_usock_posix {

    /*  The underlying stream state machine. */
    struct nn_stream stream;

    /*  State machine base class. */
    //struct nn_fsm fsm;
    //int state;

    /*  The worker thread the usock is associated with. */
    //struct nn_worker *worker;

    /*  The underlying OS socket and handle that represents it in the poller. */
    //int s;
    //struct nn_task_io wfd;

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
        struct iovec iov [NN_STREAM_MAX_IOVCNT];
    } out;

    /*  Asynchronous tasks for the worker. */
    //struct nn_task_io task_connecting;
    //struct nn_task_io task_connected;
    //struct nn_task_io task_accept;
    //struct nn_task_io task_send;
    //struct nn_task_io task_recv;
    //struct nn_task_io task_stop;

    /*  Events raised by the usock. */
    //struct nn_fsm_event established;
    //struct nn_fsm_event sent;
    //struct nn_fsm_event received;
    //struct nn_fsm_event errored;

    /*  In ACCEPTING state points to the socket being accepted.
        In BEING_ACCEPTED state points to the listener socket. */
    //struct nn_usock_posix *asock;

    /*  Errno remembered in NN_STREAM_ERROR state  */
    //int errnum;
};


/*  Private functions. */
static void nn_usock_init_from_fd (struct nn_usock_posix *self, int s);
static int nn_usock_send_raw (struct nn_usock_posix *self, struct msghdr *hdr);
static int nn_usock_recv_raw (struct nn_usock_posix *self, void *buf, size_t *len);
static int nn_usock_geterr (struct nn_usock_posix *self);
static void nn_usock_posix_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_usock_posix_shutdown (struct nn_fsm *self, int type, void *srcptr);
static void nn_usock_posix_close (struct nn_stream *s);

void nn_usock_posix_init (struct nn_usock_posix *self, struct nn_fsm *owner)
{
    /*  Initalise the state machine. */
    //nn_fsm_init (&self->fsm, nn_usock_posix_handler, nn_usock_posix_shutdown,
    //    self, owner);
    //self->state = NN_USOCK_STATE_IDLE;

    /*  Actual file descriptor will be generated during 'start' step. */
    //self->s = NN_INVALID_FD;
    //self->errnum = 0;

    self->in.buf = NULL;
    self->in.len = 0;
    self->in.batch = NULL;
    self->in.batch_len = 0;
    self->in.batch_pos = 0;
    self->in.pfd = NULL;

    memset (&self->out.hdr, 0, sizeof (self->out.hdr));

    /*  Initialise tasks for the worker thread. */
    //nn_task_io_init (&self->wfd, &self->fsm);
    //nn_task_io_init (&self->task_connecting, &self->fsm);
    //nn_task_io_init (&self->task_connected, &self->fsm);
    //nn_task_io_init (&self->task_accept, &self->fsm);
    //nn_task_io_init (&self->task_send, &self->fsm);
    //nn_task_io_init (&self->task_recv, &self->fsm);
    //nn_task_io_init (&self->task_stop, &self->fsm);

    /*  Intialise events raised by usock. */
    //nn_fsm_event_init (&self->established);
    //nn_fsm_event_init (&self->sent);
    //nn_fsm_event_init (&self->received);
    //nn_fsm_event_init (&self->errored);

    /*  accepting is not going on at the moment. */
    //self->asock = NULL;
}

void nn_usock_posix_term (struct nn_usock_posix *self)
{
    //nn_assert_state (self, NN_USOCK_STATE_IDLE);

    if (self->in.batch) {
        nn_free (self->in.batch);
    }

    //nn_fsm_event_term (&self->errored);
    //nn_fsm_event_term (&self->received);
    //nn_fsm_event_term (&self->sent);
    //nn_fsm_event_term (&self->established);

    //nn_task_io_cancel (&self->task_recv);

    //nn_task_io_term (&self->task_stop);
    //nn_task_io_term (&self->task_recv);
    //nn_task_io_term (&self->task_send);
    //nn_task_io_term (&self->task_accept);
    //nn_task_io_term (&self->task_connected);
    //nn_task_io_term (&self->task_connecting);
    //nn_task_io_term (&self->wfd);

    //nn_fsm_term (&self->fsm);
}

int nn_usock_posix_start (struct nn_usock_posix *self, int domain, int type, int protocol)
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

static void nn_usock_init_from_fd (struct nn_usock_posix *self, int s)
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

int nn_usock_posix_setsockopt (struct nn_usock_posix *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  The socket can be modified only before it's active. */
    nn_assert (self->state == NN_USOCK_STATE_STARTING ||
        self->state == NN_USOCK_STATE_ACCEPTED);

    /*  EINVAL errors are ignored on OSX platform. The reason for that is buggy
        OSX behaviour where setsockopt returns EINVAL if the peer have already
        disconnected. Thus, nn_usock_posix_setsockopt() can succeed on OSX even though
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

int nn_usock_posix_bind (struct nn_usock_posix *self, const struct sockaddr *addr,
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

int nn_usock_posix_listen (struct nn_usock_posix *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    nn_assert_state (self, NN_USOCK_STATE_STARTING);

    /*  Start listening for incoming connections. */
    rc = listen (self->s, backlog);
    if (rc != 0)
        return -errno;

    /*  Notify the state machine. */
    nn_fsm_do_now (&self->fsm, NN_STREAM_START_LISTENING);

    return 0;
}

void nn_usock_posix_accept (struct nn_usock_posix *self,
    struct nn_usock_posix *listener)
{
    int s;

    nn_fsm_do_now (&listener->fsm, NN_STREAM_START_ACCEPTING);

    /*  Start the actual accepting. */
    if (nn_fsm_isidle (&self->fsm)) {
        nn_fsm_start (&self->fsm);
        nn_fsm_do_now (&self->fsm, NN_STREAM_START_BEING_ACCEPTED);
    }

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
        nn_fsm_do_now (&listener->fsm, NN_STREAM_ACCEPTED);
        nn_fsm_do_now (&self->fsm, NN_STREAM_ACCEPTED);
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
        any errors until next IN_FD event so that we avoid a tight loop that
        would prevent processing other events in the meantime. */
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED &&
        errno != listener->errnum) {
        listener->errnum = errno;
        listener->state = NN_USOCK_STATE_ACCEPTING_ERROR;
        nn_fsm_raise (&listener->fsm, &listener->errored, NN_STREAM_ACCEPT_ERROR);
        return;
    }

    /*  Ask the worker thread to wait for the new connection. */
    nn_task_io_start (&listener->task_accept, NN_STREAM_ACCEPTED);
}

void nn_usock_posix_connect (struct nn_usock_posix *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;

    /*  Notify the state machine that we've started connecting. */
    nn_fsm_do_now (&self->fsm, NN_STREAM_START_CONNECTING);

    /* Do the connect itself. */
    rc = connect (self->s, addr, (socklen_t) addrlen);

    /* Immediate success. */
    if (rc == 0) {
        nn_fsm_do_now (&self->fsm, NN_USOCK_ACTION_DONE);
        return;
    }

    /*  Immediate error. */
    if (errno != EINPROGRESS) {
        self->errnum = errno;
        nn_fsm_do_now (&self->fsm, NN_STREAM_ERROR);
        return;
    }

    /*  Start asynchronous connect. */
    nn_task_io_start (&self->task_connecting, NN_USOCK_SRC_TASK_CONNECTING);
}

void nn_usock_posix_send (struct nn_usock_posix *self, const struct nn_iovec *iov,
    int iovcnt)
{
    int rc;
    int i;
    int out;

    /*  Make sure that the socket is actually alive. */
    if (self->state != NN_USOCK_STATE_ACTIVE) {
        nn_fsm_do_now (&self->fsm, NN_STREAM_ERROR);
        return;
    }

    /*  Copy the iovecs to the socket. */
    nn_assert (iovcnt <= NN_STREAM_MAX_IOVCNT);
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
        nn_fsm_raise (&self->fsm, &self->sent, NN_STREAM_SENT);
        return;
    }

    /*  Errors. */
    if (rc != -EAGAIN) {
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_fsm_do_now (&self->fsm, NN_STREAM_ERROR);
        return;
    }

    /*  Ask the worker thread to send the remaining data. */
    nn_task_io_start (&self->task_send, NN_STREAM_SENT);
}

void nn_usock_posix_recv (struct nn_usock_posix *self, void *buf, size_t len)
{
    int rc;
    size_t nbytes;

    /*  Make sure that the socket is actually alive. */
    if (self->state != NN_USOCK_STATE_ACTIVE) {
        nn_fsm_do_now (&self->fsm, NN_STREAM_ERROR);
        return;
    }

    /*  Try to receive the data immediately. */
    nbytes = len;
    rc = nn_usock_recv_raw (self, buf, &nbytes);
    if (rc < 0) {
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_fsm_do_now (&self->fsm, NN_STREAM_ERROR);
        return;
    }

    /*  Success. */
    if (nbytes == len) {
        nn_fsm_raise (&self->fsm, &self->received, NN_STREAM_RECEIVED);
        return;
    }

    /*  There are still data to receive in the background. */
    self->in.buf = ((uint8_t*) buf) + nbytes;
    self->in.len = len - nbytes;

    /*  Ask the worker thread to receive the remaining data. */
    nn_task_io_start (&self->task_recv, NN_STREAM_RECEIVED);
}

static int nn_usock_send_raw (struct nn_usock_posix *self, struct msghdr *hdr)
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
        if (nbytes >= (ssize_t) hdr->msg_iov->iov_len) {
            --hdr->msg_iovlen;
            if (!hdr->msg_iovlen) {
                nn_assert (nbytes == (ssize_t) hdr->msg_iov->iov_len);
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

static int nn_usock_recv_raw (struct nn_usock_posix *self, void *buf, size_t *len)
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
        self->in.batch = nn_alloc (NN_USOCK_POSIX_BATCH_SIZE, "AIO in batch");
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
    if (length > NN_USOCK_POSIX_BATCH_SIZE) {
        iov.iov_base = buf;
        iov.iov_len = length;
    }
    else {
        iov.iov_base = self->in.batch;
        iov.iov_len = NN_USOCK_POSIX_BATCH_SIZE;
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
    if (length > NN_USOCK_POSIX_BATCH_SIZE) {
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

static int nn_usock_geterr (struct nn_usock_posix *self)
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

/*  Internal tasks sent from the user thread to the worker thread. */
static int nn_internal_tasks (struct nn_usock_posix *usock, int type)
{
    switch (type) {
    case NN_STREAM_SENT:
        nn_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case NN_STREAM_RECEIVED:
        nn_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    case NN_STREAM_CONNECTED:
        nn_worker_fd_register (usock->worker, usock->s, &usock->wfd);
        return 1;
    case NN_USOCK_SRC_TASK_CONNECTING:
        nn_worker_fd_register (usock->worker, usock->s, &usock->wfd);
        nn_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case NN_STREAM_ACCEPTED:
        nn_worker_fd_register (usock->worker, usock->s, &usock->wfd);
        nn_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    }

    return 0;
}

static void nn_usock_posix_close (struct nn_stream *s)
{
    nn_closefd (s->fd);
    s->fd = NN_INVALID_FD;
}

static void nn_usock_posix_shutdown (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_usock_posix *self = nn_cont (myfsm, struct nn_usock_posix, fsm);

    if (nn_internal_tasks (self, type))
        return;

    if (type == NN_FSM_STOP) {

        /*  Stream cannot be shutdown while an actively accepting. */
        nn_assert (self->state != NN_USOCK_STATE_ACCEPTING &&
            self->state != NN_USOCK_STATE_CANCELLING_ACCEPT);

        self->errnum = 0;

        /*  Synchronous stop. */
        if (self->state == NN_USOCK_STATE_IDLE)
            goto finish3;
        if (self->state == NN_USOCK_STATE_DONE)
            goto finish2;
        if (self->state == NN_USOCK_STATE_STARTING ||
            self->state == NN_USOCK_STATE_ACCEPTED ||
            self->state == NN_USOCK_STATE_ACCEPTING_ERROR ||
            self->state == NN_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (self->state == NN_USOCK_STATE_BEING_ACCEPTED) {
            nn_fsm_do_now (&self->asock->fsm, NN_STREAM_CANCEL_ACCEPT);
            self->state = NN_USOCK_STATE_CANCELLING_ACCEPT;
            return;
        }

        /*  If we were already in the process of cancelling overlapped
            operations, we don't have to do anything. Continue waiting
            till cancelling is finished. */
        if (self->state == NN_USOCK_STATE_CANCELLING_IO) {
            self->state = NN_USOCK_STATE_STOPPING;
            return;
        }

        /*  Notify our parent that pipe socket is shutting down  */
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_SHUTDOWN);
        self->state = NN_USOCK_STATE_STOPPING;

        /*  Asynchronous stop. */
        nn_task_io_start (&self->task_stop, NN_STREAM_STOPPED);
        return;
    }
    if (self->state == NN_USOCK_STATE_CANCELLING_ACCEPT) {
        nn_assert (type == NN_STREAM_ACCEPT_ERROR);
        goto finish2;
    }
    if (self->state == NN_USOCK_STATE_STOPPING) {
        if (nn_stream_pending (self)) {
            return;
        }
        nn_worker_rm_fd (self->worker, &self->wfd);
    finish1:
        nn_usock_posix_close (self);
    finish2:
        self->state = NN_USOCK_STATE_IDLE;
        nn_fsm_stopped (&self->fsm, NN_STREAM_STOPPED);
    finish3:
        return;
    }

    nn_assert_unreachable_fsm (self->state, type);
}

static void nn_usock_posix_handler (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_usock_posix *self = nn_cont (myfsm, struct nn_usock_posix, fsm);
    int rc;
    int s;
    size_t sz;
    int sockerr;
    nn_assert (srcptr == NULL);

    if (nn_internal_tasks (self, type))
        return;

    switch (self->state | type) {

    case (NN_USOCK_STATE_IDLE | NN_FSM_START):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_STARTING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_LISTENING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_LISTENING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_CONNECTING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_CONNECTING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_BEING_ACCEPTED):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_BEING_ACCEPTED;
        return;

    case (NN_USOCK_STATE_BEING_ACCEPTED | NN_STREAM_ACCEPTED):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACCEPTED;
        nn_fsm_raise (&self->fsm, &self->established, NN_STREAM_ACCEPTED);
        return;

    case (NN_USOCK_STATE_ACCEPTED | NN_STREAM_ACCEPTED):
        nn_assert (srcptr == NULL);
        nn_worker_fd_register (self->worker, self->s, &self->wfd);
        self->state = NN_USOCK_STATE_ACTIVE;
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_CONNECTED):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACTIVE;
        nn_task_io_start (&self->task_connected, NN_STREAM_CONNECTED);
        nn_fsm_raise (&self->fsm, &self->established, NN_STREAM_CONNECTED);
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_CONNECTED):
        nn_assert (srcptr == NULL);
        nn_worker_reset_out (self->worker, &self->wfd);
        self->state = NN_USOCK_STATE_ACTIVE;
        sockerr = nn_usock_geterr (self);
        if (sockerr == 0) {
            nn_fsm_raise (&self->fsm, &self->established, NN_STREAM_CONNECTED);
        } else {
            self->errnum = sockerr;
            nn_worker_rm_fd (self->worker, &self->wfd);
            rc = close (self->s);
            errno_assert (rc == 0);
            self->s = NN_INVALID_FD;
            self->state = NN_USOCK_STATE_DONE;
            nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        }
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        nn_worker_rm_fd (self->worker, &self->wfd);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_RECEIVED):
        nn_assert (srcptr == NULL);
        sz = self->in.len;
        rc = nn_usock_recv_raw (self, self->in.buf, &sz);
        if (rc == 0) {
            self->in.len -= sz;
            self->in.buf += sz;
            if (self->in.len == 0) {
                /*  Batch receiving is complete. */
                nn_worker_reset_in (self->worker, &self->wfd);
                nn_fsm_raise (&self->fsm, &self->received, NN_STREAM_RECEIVED);
            }
            return;
        }
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_worker_rm_fd (self->worker, &self->wfd);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_SENT):
        nn_assert (srcptr == NULL);
        rc = nn_usock_send_raw (self, &self->out.hdr);
        if (rc == 0) {
            nn_worker_reset_out (self->worker, &self->wfd);
            nn_fsm_raise (&self->fsm, &self->sent, NN_STREAM_SENT);
            return;
        }
        if (rc == -EAGAIN)
            return;
        errnum_assert (rc == -ECONNRESET, -rc);
        nn_worker_rm_fd (self->worker, &self->wfd);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        nn_worker_rm_fd (self->worker, &self->wfd);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_ERROR):
        self->state = NN_USOCK_STATE_CANCELLING_IO;
        nn_task_io_start (&self->task_stop, NN_STREAM_STOPPED);
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_SHUTDOWN);
        return;

    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_STOPPED):
        nn_assert (srcptr == NULL);
        nn_worker_rm_fd (self->worker, &self->wfd);
        nn_usock_posix_close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_RECEIVED):
    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_SENT):
        nn_assert (srcptr == NULL);
        /*  Lingering I/O events from worker are ignored during shutdown. */
        return;

    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        return;

    
    case (NN_USOCK_STATE_LISTENING | NN_STREAM_START_ACCEPTING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACCEPTING;
        return;

    case (NN_USOCK_STATE_ACCEPTING | NN_STREAM_ACCEPTED):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_LISTENING;
        return;

    case (NN_USOCK_STATE_ACCEPTING | NN_STREAM_CANCEL_ACCEPT):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_CANCELLING_ACCEPT;
        nn_task_io_start (&self->task_stop, NN_STREAM_STOPPED);
        return;

    case (NN_USOCK_STATE_ACCEPTING | NN_STREAM_ACCEPTED):
        nn_assert (srcptr == NULL);
        /*  New connection arrived in asynchronous manner. */
#if NN_HAVE_ACCEPT4
        s = accept4 (self->s, NULL, NULL, SOCK_CLOEXEC);
#else
        s = accept (self->s, NULL, NULL);
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
            self->errnum = errno;
            self->state = NN_USOCK_STATE_ACCEPTING_ERROR;

            /*  Wait till the user starts accepting once again. */
            nn_worker_rm_fd (self->worker, &self->wfd);

            nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ACCEPT_ERROR);
            return;
        }

        /*  Any other error is unexpected. */
        errno_assert (s >= 0);

        /*  Initialise the new usock object. */
        nn_usock_init_from_fd (self->asock, s);
        self->asock->state = NN_USOCK_STATE_ACCEPTED;

        /*  Notify the user that connection was accepted. */
        nn_fsm_raise (&self->asock->fsm, &self->asock->established, NN_STREAM_ACCEPTED);

        /*  Disassociate the listener socket from the accepted
            socket. */
        self->asock->asock = NULL;
        self->asock = NULL;

        /*  Wait till the user starts accepting once again. */
        nn_worker_rm_fd (self->worker, &self->wfd);
        self->state = NN_USOCK_STATE_LISTENING;

        return;

    case (NN_USOCK_STATE_ACCEPTING_ERROR | NN_STREAM_START_ACCEPTING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACCEPTING;
        return;

    case (NN_USOCK_STATE_CANCELLING_ACCEPT | NN_STREAM_STOPPED):
        nn_assert (srcptr == NULL);
        nn_worker_rm_fd (self->worker, &self->wfd);
        self->state = NN_USOCK_STATE_LISTENING;

        /*  Notify the accepted socket that it was stopped. */
        nn_fsm_do_now (&self->asock->fsm, NN_STREAM_ACCEPT_ERROR);

        return;

    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_RECEIVED):
    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_SENT):
        nn_assert (srcptr == NULL);
        return;

    default:
        nn_assert_unreachable_fsm (self->state, type);
        return;
    }
}

#endif
