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

#include "tcp.h"

#include "../../nn.h"

#include "../../tcp.h"

#include "../stream/ustream.h"
#include "../stream/bstream.h"
#include "../stream/cstream.h"

#include "../../aio/fsm.h"
#include "../../aio/worker.h"

#include "../utils/dns.h"
#include "../utils/literal.h"
#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../utils/err.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/cont.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
//#include <netinet/in.h>
//#include <netinet/tcp.h>
#endif

/*  The backlog is set relatively high so that there are not too many failed
    connection attempts during re-connection storms. */
#define NN_TCP_LISTEN_BACKLOG 100

/*  Transport-specific socket options. */
struct nn_tcp_optset {
    struct nn_optset base;
    int nodelay;
};

static void nn_tcp_optset_destroy (struct nn_optset *self);
static int nn_tcp_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen);
static int nn_tcp_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct nn_optset_vfptr nn_tcp_optset_vfptr = {
    nn_tcp_optset_destroy,
    nn_tcp_optset_setopt,
    nn_tcp_optset_getopt
};

/*  nn_transport interface. */
static int nn_tcp_bind (void *hint, struct nn_epbase **epbase);
static int nn_tcp_connect (void *hint, struct nn_epbase **epbase);
static struct nn_optset *nn_tcp_optset (void);

static struct nn_transport nn_tcp_vfptr = {
    "tcp",
    NN_TCP,
    NULL,
    NULL,
    nn_tcp_bind,
    nn_tcp_connect,
    nn_tcp_optset,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_tcp = &nn_tcp_vfptr;

struct nn_atcp {
    struct nn_astream base;
};

struct nn_btcp {
    struct nn_bstream base;
};

struct nn_ctcp {
    struct nn_cstream base;

    /*  DNS resolver used to convert textual address into actual IP address
        along with the variable to hold the result. */
    struct nn_dns dns;
    struct nn_dns_result dns_result;
};

static int nn_utcp_sent (struct nn_stream *s);
static int nn_utcp_cancel_io (struct nn_stream *s);
static int nn_utcp_start_resolve (struct nn_cstream *cstream);
static int nn_utcp_start_connect (struct nn_cstream *cstream);
static int nn_utcp_start_listen (struct nn_stream *s, struct nn_epbase *e);
static int nn_utcp_tune (struct nn_stream *s, struct nn_epbase *e);
static int nn_utcp_activate (struct nn_astream *as);
static int nn_utcp_close (struct nn_stream *s);

static struct nn_stream_vfptr nn_stream_vfptr_tcp = {
    nn_utcp_sent,
    nn_utcp_cancel_io,
    nn_utcp_start_resolve,
    nn_utcp_start_connect,
    nn_utcp_start_listen,
    nn_utcp_tune,
    nn_utcp_activate,
    nn_utcp_close
};






static int nn_tcp_bind (void *hint, struct nn_epbase **epbase)
{
    struct nn_btcp *self;
    struct sockaddr_storage ss;
    size_t sslen;
    size_t ipv4onlylen;
    int ipv4only;
    const char *addr;
    const char *end;
    const char *pos;
    int rc;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (*self), "btcp");
    nn_assert_alloc (self);

    /*  Begin parsing the address. */
    addr = nn_epbase_getaddr (*epbase);

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    if (!pos) {
        return -EINVAL;
    }
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    if (rc < 0) {
        return -EINVAL;
    }

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (*epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the address. */
    rc = nn_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    if (rc < 0) {
        return -ENODEV;
    }

    return nn_bstream_create (&self->base, hint, epbase, &nn_stream_vfptr_tcp);
}

static int nn_tcp_connect (void *hint, struct nn_epbase **epbase)
{
    struct nn_ctcp *self;
    const char *addr;
    size_t addrlen;
    const char *semicolon;
    const char *hostname;
    const char *colon;
    const char *end;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    int rc;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (*self), "ctcp");
    nn_assert_alloc (self);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (*epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Start parsing the address. */
    addr = nn_epbase_getaddr (*epbase);
    addrlen = strlen (addr);
    semicolon = strchr (addr, ';');
    hostname = semicolon ? semicolon + 1 : addr;
    colon = strrchr (addr, ':');
    end = addr + addrlen;

    /*  Parse the port. */
    if (!colon) {
        nn_epbase_term (*epbase);
        return -EINVAL;
    }
    rc = nn_port_resolve (colon + 1, end - colon - 1);
    if (rc < 0) {
        nn_epbase_term (*epbase);
        return -EINVAL;
    }

    /*  Check whether the host portion of the address is either a literal
        or a valid hostname. */
    if (nn_dns_check_hostname (hostname, colon - hostname) < 0 &&
        nn_literal_resolve (hostname, colon - hostname, ipv4only,
            &ss, &sslen) < 0) {
        nn_epbase_term (*epbase);
        return -EINVAL;
    }

    /*  If local address is specified, check whether it is valid. */
    if (semicolon) {
        rc = nn_iface_resolve (addr, semicolon - addr, ipv4only, &ss, &sslen);
        if (rc < 0) {
            nn_epbase_term (*epbase);
            return -ENODEV;
        }
    }

    return nn_cstream_create (&self->base, hint, epbase, &nn_stream_vfptr_tcp);
}

static struct nn_optset *nn_tcp_optset ()
{
    struct nn_tcp_optset *optset;

    optset = nn_alloc (sizeof (struct nn_tcp_optset), "optset (tcp)");
    nn_assert_alloc (optset);
    optset->base.vfptr = &nn_tcp_optset_vfptr;

    /*  Default values for TCP socket options. */
    optset->nodelay = 0;

    return &optset->base;   
}

static void nn_tcp_optset_destroy (struct nn_optset *self)
{
    struct nn_tcp_optset *optset;

    optset = nn_cont (self, struct nn_tcp_optset, base);
    nn_free (optset);
}

static int nn_tcp_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct nn_tcp_optset *optset;
    int val;

    optset = nn_cont (self, struct nn_tcp_optset, base);

    /*  At this point we assume that all options are of type int. */
    if (optvallen != sizeof (int))
        return -EINVAL;
    val = *(int*) optval;

    switch (option) {
    case NN_TCP_NODELAY:
        if (val != 0 && val != 1)
            return -EINVAL;
        optset->nodelay = val;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_tcp_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct nn_tcp_optset *optset;
    int intval;

    optset = nn_cont (self, struct nn_tcp_optset, base);

    switch (option) {
    case NN_TCP_NODELAY:
        intval = optset->nodelay;
        break;
    default:
        return -ENOPROTOOPT;
    }
    memcpy (optval, &intval,
        *optvallen < sizeof (int) ? *optvallen : sizeof (int));
    *optvallen = sizeof (int);
    return 0;
}

#if defined NN_HAVE_WINDOWS

struct nn_utcp {

    /*  The underlying stream state machine. */
    struct nn_stream stream;

    /*  When accepting new socket, they have to be created with same
        type as the listening socket. Thus, in listening socket we
        have to store its exact type. */
    int domain;
    int type;
    int protocol;

    /*  Buffer allocated for output of AcceptEx function. */
    char ainfo [512];
};


void nn_utcp_init (struct nn_utcp *self, struct nn_fsm *owner)
{
    nn_stream_init (&self->stream, owner, &nn_stream_vfptr_tcp);

    self->domain = -1;
    self->type = -1;
    self->protocol = -1;

    memset (self->ainfo, 0, sizeof (self->ainfo));
}

void nn_utcp_term (struct nn_utcp *self)
{
    nn_stream_term (&self->stream);
}

int nn_utcp_start (struct nn_utcp *self, int domain, int type, int protocol)
{
    SOCKET s;
    DWORD only;
    BOOL nodelay;
    int rc;

    /*  Open the underlying socket. */
    s = socket (domain, type, protocol);
    if (s == INVALID_SOCKET)
        return -nn_err_wsa_to_posix (WSAGetLastError ());

    /*  IPv4 mapping for IPv6 sockets is disabled by default. Switch it on. */
    if (domain == AF_INET6) {
        only = 0;
        rc = setsockopt (s, IPPROTO_IPV6, IPV6_V6ONLY,
            (const char*) &only, sizeof (only));
        nn_assert_win (rc != SOCKET_ERROR);
    }

    /*  By default, ensure Nagle's algorithm for batched sends is off. */
    nodelay = TRUE;
    rc = setsockopt (s, IPPROTO_TCP, TCP_NODELAY,
        (const char*) &nodelay, sizeof (nodelay));
    nn_assert_win (rc != SOCKET_ERROR);

    /*  Remember the type of the socket. */
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;

    /*  Associate the socket with a worker thread/completion port. */
    nn_worker_fd_register (&self->stream, s);

    /*  Start the state machine. */
    nn_fsm_start (&self->stream.fsm);

    return 0;
}

int nn_utcp_setsockopt (struct nn_utcp *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  The socket can be modified only before it's active. */
    nn_assert (self->stream.state == NN_USOCK_STATE_STARTING ||
        self->stream.state == NN_USOCK_STATE_ACCEPTED);

    nn_assert (optlen < INT_MAX);

    if (level == SOL_SOCKET && optname == SO_SNDBUF) {

    }

    //nn_utcp_setsockopt (&self->usock, SOL_SOCKET, SO_SNDBUF,
    //    &val, sizeof (val));

    rc = setsockopt (self->stream.fd, level, optname, (char*) optval, (int) optlen);
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
    nn_assert_state (&self->stream, NN_USOCK_STATE_STARTING);

    /*  On Windows, the bound port can be hijacked
    if SO_EXCLUSIVEADDRUSE is not set. */
    opt = 1;
    rc = setsockopt (self->stream.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        (const char*) &opt, sizeof (opt));
    nn_assert_win (rc != SOCKET_ERROR);

    nn_assert (addrlen < INT_MAX);
    rc = bind (self->stream.fd, addr, (int) addrlen);
    if (rc == SOCKET_ERROR)
        return -nn_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int nn_utcp_listen (struct nn_utcp *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    nn_assert_state (&self->stream, NN_USOCK_STATE_STARTING);

    rc = listen (self->stream.fd, backlog);
    if (rc == SOCKET_ERROR) {
        return -nn_err_wsa_to_posix (WSAGetLastError ());
    }

    /*  Notify the state machine. */
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_LISTENING);

    return 0;
}

static int nn_tcp_start_listen (struct nn_stream *s, struct nn_epbase *e)
{
    const char *addr;
    int rc;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    const char *end;
    const char *pos;
    uint16_t port;

    /*  First, resolve the IP address. */
    addr = nn_epbase_getaddr (e);
    memset (&ss, 0, sizeof (ss));

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    if (pos == NULL) {
        return -EINVAL;
    }
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    if (rc <= 0) {
        return rc;
    }
    port = (uint16_t) rc;

    /*  Parse the address. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (e, NN_SOL_SOCKET, NN_IPV4ONLY, &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));
    rc = nn_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    if (rc < 0) {
        return rc;
    }

    /*  Combine the port and the address. */
    switch (ss.ss_family) {
    case AF_INET:
        ((struct sockaddr_in*) &ss)->sin_port = htons (port);
        sslen = sizeof (struct sockaddr_in);
        break;
    case AF_INET6:
        ((struct sockaddr_in6*) &ss)->sin6_port = htons (port);
        sslen = sizeof (struct sockaddr_in6);
        break;
    default:
        nn_assert_unreachable ("Unexpected ss_family.");
    }

    /*  Start listening for incoming connections. */
    rc = nn_stream_start (s, ss.ss_family, SOCK_STREAM, 0);
    if (rc < 0) {
        return rc;
    }

    rc = nn_utcp_bind (s, (struct sockaddr *) &ss, sslen);
    if (rc < 0) {
        nn_utcp_stop (s);
        return rc;
    }

    rc = nn_utcp_listen (s, NN_TCP_LISTEN_BACKLOG);
    if (rc < 0) {
        nn_utcp_stop (s);
        return rc;
    }

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
    nn_fsm_do_now (&listener->stream.fsm, NN_STREAM_START_ACCEPTING);
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_BEING_ACCEPTED);

    /*  Wait for the incoming connection. */
    nn_task_io_start (&listener->stream.incoming, NN_STREAM_ACCEPTED);
    brc = AcceptEx (listener->stream.fd, self->stream.fd, listener->ainfo, 0,
        256, 256, &nbytes, &listener->stream.incoming.olpd);
    err = brc ? ERROR_SUCCESS : WSAGetLastError();

    /*  Pair the two sockets. */
    nn_assert (self->stream.asock == NULL);
    self->stream.asock = &listener->stream.fsm;
    nn_assert (listener->stream.asock == NULL);
    listener->stream.asock = &self->stream.fsm;

    /*  This is the most likely return path with overlapped I/O. */
    if (err == WSA_IO_PENDING) {


        /*  Asynchronous accept. */

        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");

        nn_fsm_do_now (&listener->stream.fsm, NN_STREAM_ACCEPTED);
        nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ACCEPTED);
        return;
    }

    nn_assert_unreachable ("TODO: determine cases when this can fail.");
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
    nn_assert_state (&self->stream, NN_USOCK_STATE_STARTING);

    /*  Notify the state machine that we've started connecting. */
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_START_CONNECTING);

    /*  Get the pointer to connect function. */
    brc = WSAIoctl (self->stream.fd, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &fid, sizeof (fid), &pconnectex, sizeof (pconnectex),
        &nbytes, NULL, NULL) == 0;
    nn_assert_win (brc == TRUE && nbytes == sizeof (pconnectex));

    /*  Ensure it is safe to cast this value to what might be a smaller
    integer type to conform to the pconnectex function signature. */
    nn_assert (addrlen < INT_MAX);

    /*  Begin connecting. */
    nn_task_io_start (&self->stream.outgoing, NN_STREAM_CONNECTED);
    brc = pconnectex (self->stream.fd, addr, (int) addrlen, NULL, 0, NULL,
        &self->stream.outgoing.olpd);
    err = brc ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Most likely return path is asynchronous connect. */
    if (err == WSA_IO_PENDING) {
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");

        nn_fsm_do_now (&self->stream.fsm, NN_STREAM_CONNECTED);
        return;
    }

    /*  Unkown error. */
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
    return;
}

void nn_utcp_send (struct nn_utcp *self, const struct nn_iovec *iov,
    int iovcnt)
{
    WSABUF wbuf [NN_STREAM_MAX_IOVCNT];
    DWORD err;
    size_t len;
    int rc;
    int i;

    /*  Make sure that the socket is actually alive. */
    nn_assert_state (&self->stream, NN_USOCK_STATE_ACTIVE);

    /*  Create a WinAPI-style iovec. */
    len = 0;
    nn_assert (iovcnt <= NN_STREAM_MAX_IOVCNT);
    for (i = 0; i != iovcnt; ++i) {
        wbuf [i].buf = (char FAR*) iov [i].iov_base;
        wbuf [i].len = (ULONG) iov [i].iov_len;
        len += iov [i].iov_len;
    }

    /*  Start the send operation. */
    nn_task_io_start (&self->stream.outgoing, NN_STREAM_SENT);
    rc = WSASend (self->stream.fd, wbuf, iovcnt, NULL, 0,
        &self->stream.outgoing.olpd, NULL);
    err = (rc == 0) ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Async send. */
    if (err == WSA_IO_PENDING) {
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");
        return;
    }

    /*  Set of expected errors. */
    nn_assert_win (err == WSAECONNABORTED || err == WSAECONNRESET ||
        err == WSAENETDOWN || err == WSAENETRESET ||
        err == WSAENOTCONN || err == WSAENOBUFS || err == WSAEWOULDBLOCK);
    self->stream.err = nn_err_wsa_to_posix (err);
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
}

void nn_utcp_recv (struct nn_utcp *self, void *buf, size_t len)
{
    DWORD wflags;
    WSABUF wbuf;
    int err;
    int rc;

    /*  Make sure that the socket is actually alive. */
    nn_assert_state (&self->stream, NN_USOCK_STATE_ACTIVE);

    /*  Start the receive operation. */
    wbuf.len = (ULONG) len;
    wbuf.buf = (char FAR*) buf;
    wflags = MSG_WAITALL;
    nn_task_io_start (&self->stream.incoming, NN_STREAM_RECEIVED);
    rc = WSARecv (self->stream.fd, &wbuf, 1, NULL, &wflags,
        &self->stream.incoming.olpd, NULL);
    err = (rc == 0) ? ERROR_SUCCESS : WSAGetLastError ();

    /*  Async receive. */
    if (err == WSA_IO_PENDING) {
        return;
    }

    /*  Immediate success. */
    if (err == ERROR_SUCCESS) {

        nn_assert_unreachable ("JRD - do we actually hit this?");
        return;
    }

    /*  Set of expected errors. */
    nn_assert (err == WSAECONNABORTED || err == WSAECONNRESET ||
        err == WSAEDISCON || err == WSAENETDOWN || err == WSAENETRESET ||
        err == WSAENOTCONN || err == WSAETIMEDOUT || err == WSAEWOULDBLOCK);
    nn_fsm_do_now (&self->stream.fsm, NN_STREAM_ERROR);
}

static int nn_utcp_activate (struct nn_astream *as)
{
    return 0;
}

int nn_utcp_cancel_io (struct nn_stream *s)
{
    nn_task_io_cancel (&s->incoming);
    nn_task_io_cancel (&s->outgoing);

    return 1;
}

static int nn_utcp_close (struct nn_stream *s)
{
    int rc;

    rc = closesocket (s->fd);
    nn_assert_win (rc == 0);
    s->fd = NN_INVALID_FD;

    return 0;
}

static void nn_tcp_start_resolve (struct nn_cstream *cs)
{
    struct nn_ctcp *self = nn_cont (cs, struct nn_ctcp, base);
    const char *addr;
    const char *begin;
    const char *end;
    int ipv4only;
    size_t ipv4onlylen;

    /*  Extract the hostname part from address string. */
    addr = nn_epbase_getaddr (&self->base.epbase);
    begin = strchr (addr, ';');
    if (!begin)
        begin = addr;
    else
        ++begin;
    end = strrchr (addr, ':');
    nn_assert (end);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->base.epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  TODO: Get the actual value of IPV4ONLY option. */
    nn_dns_start (&self->dns, begin, end - begin, ipv4only, &self->dns_result);
}

static int nn_tcp_start_connect (struct nn_cstream *cs)
{
    struct nn_ctcp *self = nn_cont (cs, struct nn_ctcp, base);
    int rc;
    struct sockaddr_storage remote;
    struct sockaddr_storage local;
    size_t locallen;
    const char *addr;
    const char *end;
    const char *colon;
    const char *semicolon;
    uint16_t port;
    int ipv4only;
    size_t ipv4onlylen;
    int val;
    size_t sz;

    /*  Create IP address from the address string. */
    addr = nn_epbase_getaddr (&self->base.epbase);
    memset (&remote, 0, sizeof (remote));

    /*  Parse the port. */
    end = addr + strlen (addr);
    colon = strrchr (addr, ':');
    rc = nn_port_resolve (colon + 1, end - colon - 1);
    errnum_assert (rc > 0, -rc);
    port = rc;

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&cs->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the local address, if any. */
    semicolon = strchr (addr, ';');
    memset (&local, 0, sizeof (local));
    if (semicolon) {
        rc = nn_iface_resolve (addr, semicolon - addr, ipv4only,
            &local, &locallen);
    } else {
        rc = nn_iface_resolve ("*", 1, ipv4only, &local, &locallen);
    }
    if (rc < 0) {
        return rc;
    }

    /*  Combine the remote address and the port. */
    remote = *ss;
    if (remote.ss_family == AF_INET)
        ((struct sockaddr_in*) &remote)->sin_port = htons (port);
    else if (remote.ss_family == AF_INET6)
        ((struct sockaddr_in6*) &remote)->sin6_port = htons (port);
    else
        nn_assert_unreachable ("Unexpected ss_family.");

    /*  Try to start the underlying socket. */
    rc = nn_stream_start (&cs->usock, remote.ss_family, SOCK_STREAM, 0);
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

    /*  Bind the socket to the local network interface. */
    rc = nn_stream_bind (&cs->usock, (struct sockaddr*) &local, locallen);
    if (rc != 0) {
        return rc;
    }

    /*  Start connecting. */
    nn_stream_connect (&cs->usock, (struct sockaddr*) &remote, sslen);
}

int nn_tcp_tune (struct nn_stream *s, struct nn_epbase *e)
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
