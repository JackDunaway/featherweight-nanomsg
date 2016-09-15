/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

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

#include "ws.h"

#include "../../nn.h"

#include "../../ws.h"

#include "../stream/ustream.h"
#include "../stream/bstream.h"
#include "../stream/cstream.h"

#include "../../aio/fsm.h"
#include "../../aio/worker.h"

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
#endif


#include "ws_handshake.h"

#include "../../utils/msg.h"
#include "../../utils/list.h"

/*  This state machine handles WebSocket connection from the point where it is
    established to the point when it is broken. */

/*  Return codes of this state machine. */
#define NN_SWS_RETURN_ERROR 1
#define NN_SWS_RETURN_CLOSE_HANDSHAKE 2
#define NN_SWS_RETURN_STOPPED 3

/*  WebSocket protocol header frame sizes. */
#define NN_SWS_FRAME_SIZE_INITIAL 2
#define NN_SWS_FRAME_SIZE_PAYLOAD_0 0
#define NN_SWS_FRAME_SIZE_PAYLOAD_16 2
#define NN_SWS_FRAME_SIZE_PAYLOAD_63 8
#define NN_SWS_FRAME_SIZE_MASK 4

/*  WebSocket control bitmasks as per RFC 6455 5.2. */
#define NN_SWS_FRAME_BITMASK_FIN 0x80
#define NN_SWS_FRAME_BITMASK_RSV1 0x40
#define NN_SWS_FRAME_BITMASK_RSV2 0x20
#define NN_SWS_FRAME_BITMASK_RSV3 0x10
#define NN_SWS_FRAME_BITMASK_OPCODE 0x0F

/*  UTF-8 validation. */
#define NN_SWS_UTF8_MAX_CODEPOINT_LEN 4

/*  The longest possible header frame length. As per RFC 6455 5.2:
    first 2 bytes of initial framing + up to 8 bytes of additional
    extended payload length header + 4 byte mask = 14bytes
    Not all messages will use the maximum amount allocated, but
    statically allocating this buffer for convenience. */
#define NN_SWS_FRAME_MAX_HDR_LEN 14

/*  WebSocket protocol payload length framing RFC 6455 section 5.2. */
#define NN_SWS_PAYLOAD_MAX_LENGTH 125
#define NN_SWS_PAYLOAD_MAX_LENGTH_16 65535
#define NN_SWS_PAYLOAD_MAX_LENGTH_63 9223372036854775807
#define NN_SWS_PAYLOAD_FRAME_16 0x7E
#define NN_SWS_PAYLOAD_FRAME_63 0x7F

/*  WebSocket Close Status Code length. */
#define NN_SWS_CLOSE_CODE_LEN 2

struct nn_sws {

    /*  The underlying stream state machine. */
    struct nn_stream stream;

    /*  Endpoint base. */
    struct nn_epbase *epbase;

    /*  Default message type set on outbound frames. */
    uint8_t msg_type;

    /*  Controls Tx/Rx framing based on whether this peer is acting as
        a Client or a Server. */
    int mode;

    /*  The underlying socket. */
    struct nn_stream *usock;

    /*  Child state machine to do protocol header exchange. */
    struct nn_ws_handshake handshaker;

    /*  The original owner of the underlying socket. */
    struct nn_fsm *owner;

    /*  Pipe connecting this WebSocket connection to the nanomsg core. */
    struct nn_pipebase pipebase;

    /*  Requested resource when acting as client. */
    const char* resource;

    /*  Remote Host in header request when acting as client. */
    const char* remote_host;

    /*  State of inbound state machine. */
    int instate;

    /*  Buffer used to store the framing of incoming message. */
    uint8_t inhdr [NN_SWS_FRAME_MAX_HDR_LEN];

    /*  Parsed header frames. */
    uint8_t opcode;
    uint8_t payload_ctl;
    uint8_t masked;
    uint8_t *mask;
    size_t ext_hdr_len;
    int is_final_frame;
    int is_control_frame;

    /*  As valid fragments are being received, this flag stays true until
        the FIN bit is received. This state is also used to determine
        peer sequencing anamolies that trigger this endpoint to fail the
        connection. */
    int continuing;

    /*  When validating continuation frames of UTF-8, it may be necessary
        to buffer tail-end of the previous frame in order to continue
        validation in the case that frames are chopped on intra-code point
        boundaries. */
    uint8_t utf8_code_pt_fragment [NN_SWS_UTF8_MAX_CODEPOINT_LEN];
    size_t utf8_code_pt_fragment_len;

    /*  Statistics on control frames. */
    int pings_sent;
    int pongs_sent;
    int pings_received;
    int pongs_received;

    /*  Fragments of message being received at the moment. */
    struct nn_list inmsg_array;
    uint8_t *inmsg_current_chunk_buf;
    size_t inmsg_current_chunk_len;
    size_t inmsg_total_size;
    int inmsg_chunks;
    uint8_t inmsg_hdr;

    /*  Control message being received at the moment. Because these can be
        interspersed between fragmented TEXT and BINARY messages, they are
        stored in this buffer so as not to interrupt the message array. */
    uint8_t inmsg_control [NN_SWS_PAYLOAD_MAX_LENGTH];

    /*  Reason this connection is closing to send as closing handshake. */
    char fail_msg [NN_SWS_PAYLOAD_MAX_LENGTH];
    size_t fail_msg_len;

    /*  State of the outbound state machine. */
    int outstate;

    /*  Buffer used to store the header of outgoing message. */
    uint8_t outhdr [NN_SWS_FRAME_MAX_HDR_LEN];

    /*  Message being sent at the moment. */
    struct nn_msg outmsg;

    /*  Event raised when the state machine ends. */
    struct nn_fsm_event done;
};

struct nn_aws {
    struct nn_astream base;
};

struct nn_bws {
    struct nn_bstream base;
};

struct nn_cws {
    struct nn_cstream base;

    /*  DNS resolver used to convert textual address into actual IP address
        along with the variable to hold the result. */
    struct nn_dns dns;
    struct nn_dns_result dns_result;
};

/*  Scatter/gather array element type for incoming message chunks. Fragmented
    message frames are reassembled prior to notifying the user. */
struct msg_chunk {
    struct nn_list_item item;
    struct nn_chunkref chunk;
};


/*  WebSocket-specific socket options. */
struct nn_ws_optset {
    struct nn_optset base;
    int msg_type;
};

static void nn_ws_optset_destroy (struct nn_optset *self);
static int nn_ws_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen);
static int nn_ws_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct nn_optset_vfptr nn_ws_optset_vfptr = {
    nn_ws_optset_destroy,
    nn_ws_optset_setopt,
    nn_ws_optset_getopt
};

/*  nn_transport interface. */
static int nn_ws_bind (void *hint, struct nn_epbase **epbase);
static int nn_ws_connect (void *hint, struct nn_epbase **epbase);
static struct nn_optset *nn_ws_optset (void);

static struct nn_transport nn_ws_vfptr = {
    "ws",
    NN_WS,
    NULL,
    NULL,
    nn_ws_bind,
    nn_ws_connect,
    nn_ws_optset,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_ws = &nn_ws_vfptr;

/*  nn_epbase virtual interface implementation. */
static void nn_cws_stop (struct nn_epbase *self);
static void nn_cws_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_cws_epbase_vfptr = {
    nn_cws_stop,
    nn_cws_destroy
};
static int nn_ws_bind (void *hint, struct nn_epbase **epbase)
{
    struct nn_bws *self;
    int rc;
    const char *addr;
    const char *end;
    const char *pos;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_bws), "bws");
    nn_assert_alloc (self);

    /*  Initalise the epbase. */
    addr = nn_epbase_getaddr (&self->epbase);

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    if (!pos) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    if (rc < 0) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the address. */
    rc = nn_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    if (rc < 0) {
        nn_epbase_term (&self->epbase);
        return -ENODEV;
    }

    return nn_bws_create (hint, epbase);
}

static int nn_ws_connect (void *hint, struct nn_epbase **epbase)
{
    int rc;
    const char *addr;
    size_t addrlen;
    const char *semicolon;
    const char *hostname;
    size_t hostlen;
    const char *colon;
    const char *slash;
    const char *resource;
    size_t resourcelen;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    struct nn_cws *self;
    int reconnect_ivl;
    int reconnect_ivl_max;
    int msg_type;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = nn_alloc (sizeof (struct nn_cws), "cws");
    nn_assert_alloc (self);

    /*  Initalise the endpoint. */
    nn_epbase_init (&self->epbase, &nn_cws_epbase_vfptr, hint);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Start parsing the address. */
    addr = nn_epbase_getaddr (&self->epbase);
    addrlen = strlen (addr);
    semicolon = strchr (addr, ';');
    hostname = semicolon ? semicolon + 1 : addr;
    colon = strrchr (addr, ':');
    slash = colon ? strchr (colon, '/') : strchr (addr, '/');
    resource = slash ? slash : addr + addrlen;
    self->remote_hostname_len = colon ? colon - hostname : resource - hostname;

    /*  Host contains both hostname and port. */
    hostlen = resource - hostname;

    /*  Parse the port; assume port 80 if not explicitly declared. */
    if (colon != NULL) {
        rc = nn_port_resolve (colon + 1, resource - colon - 1);
        if (rc < 0) {
            nn_epbase_term (&self->epbase);
            return -EINVAL;
        }
        self->remote_port = rc;
    }
    else {
        self->remote_port = 80;
    }

    /*  Check whether the host portion of the address is either a literal
    or a valid hostname. */
    if (nn_dns_check_hostname (hostname, self->remote_hostname_len) < 0 &&
        nn_literal_resolve (hostname, self->remote_hostname_len, ipv4only,
            &ss, &sslen) < 0) {
        nn_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  If local address is specified, check whether it is valid. */
    if (semicolon) {
        rc = nn_iface_resolve (addr, semicolon - addr, ipv4only, &ss, &sslen);
        if (rc < 0) {
            nn_epbase_term (&self->epbase);
            return -ENODEV;
        }
    }

    /*  At this point, the address is valid, so begin allocating resources. */
    nn_chunkref_init (&self->remote_host, hostlen + 1);
    memcpy (nn_chunkref_data (&self->remote_host), hostname, hostlen);
    ((uint8_t *) nn_chunkref_data (&self->remote_host)) [hostlen] = '\0';

    if (semicolon) {
        nn_chunkref_init (&self->nic, semicolon - addr);
        memcpy (nn_chunkref_data (&self->nic),
            addr, semicolon - addr);
    }
    else {
        nn_chunkref_init (&self->nic, 1);
        memcpy (nn_chunkref_data (&self->nic), "*", 1);
    }

    /*  The requested resource is used in opening handshake. */
    resourcelen = strlen (resource);
    if (resourcelen) {
        nn_chunkref_init (&self->resource, resourcelen + 1);
        strncpy (nn_chunkref_data (&self->resource),
            resource, resourcelen + 1);
    }
    else {
        /*  No resource specified, so allocate base path. */
        nn_chunkref_init (&self->resource, 2);
        strncpy (nn_chunkref_data (&self->resource), "/", 2);
    }

    /*  Initialise the structure. */
    nn_fsm_init_root (&self->fsm, nn_cws_handler, nn_cws_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_CWS_STATE_IDLE;
    nn_utcp_init (&self->usock, &self->fsm);

    sz = sizeof (msg_type);
    nn_epbase_getopt (&self->epbase, NN_WS, NN_WS_MSG_TYPE,
        &msg_type, &sz);
    nn_assert (sz == sizeof (msg_type));
    self->msg_type = (uint8_t) msg_type;

    sz = sizeof (reconnect_ivl);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL,
        &reconnect_ivl, &sz);
    nn_assert (sz == sizeof (reconnect_ivl));
    sz = sizeof (reconnect_ivl_max);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RECONNECT_IVL_MAX,
        &reconnect_ivl_max, &sz);
    nn_assert (sz == sizeof (reconnect_ivl_max));
    if (reconnect_ivl_max == 0)
        reconnect_ivl_max = reconnect_ivl;
    nn_backoff_init (&self->retry, reconnect_ivl, reconnect_ivl_max,
        &self->fsm);

    nn_sws_init (&self->sws, &self->epbase, &self->fsm);
    nn_dns_init (&self->dns, &self->fsm);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_cws_destroy (struct nn_epbase *self)
{
    struct nn_cws *cws;

    cws = nn_cont (self, struct nn_cws, epbase);

    nn_chunkref_term (&cws->resource);
    nn_chunkref_term (&cws->remote_host);
    nn_chunkref_term (&cws->nic);
    nn_dns_term (&cws->dns);
    nn_sws_term (&cws->sws);
    nn_backoff_term (&cws->retry);
    nn_utcp_term (&cws->usock);
    nn_fsm_term (&cws->fsm);
    nn_epbase_term (&cws->epbase);

    nn_free (cws);
}

static struct nn_optset *nn_ws_optset ()
{
    struct nn_ws_optset *optset;

    optset = nn_alloc (sizeof (struct nn_ws_optset), "optset (ws)");
    nn_assert_alloc (optset);
    optset->base.vfptr = &nn_ws_optset_vfptr;

    /*  Default values for WebSocket options. */
    optset->msg_type = NN_WS_MSG_TYPE_BINARY;

    return &optset->base;   
}

static void nn_ws_optset_destroy (struct nn_optset *self)
{
    struct nn_ws_optset *optset;

    optset = nn_cont (self, struct nn_ws_optset, base);
    nn_free (optset);
}

static int nn_ws_optset_setopt (struct nn_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct nn_ws_optset *optset;
    int val;

    optset = nn_cont (self, struct nn_ws_optset, base);
    if (optvallen != sizeof (int)) {
        return -EINVAL;
    }
    val = *(int *)optval;

    switch (option) {
    case NN_WS_MSG_TYPE:
        switch (val) {
        case NN_WS_MSG_TYPE_TEXT:
        case NN_WS_MSG_TYPE_BINARY:
	    optset->msg_type = val;
            return 0;
        default:
            return -EINVAL;
        }
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_ws_optset_getopt (struct nn_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct nn_ws_optset *optset;

    optset = nn_cont (self, struct nn_ws_optset, base);

    switch (option) {
    case NN_WS_MSG_TYPE:
        memcpy (optval, &optset->msg_type,
            *optvallen < sizeof (int) ? *optvallen : sizeof (int));
        *optvallen = sizeof (int);
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int nn_ws_start_resolve (struct nn_cstream *cstream)
{
    size_t ipv4onlylen;
    int ipv4only;
    char *host;

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (e, NN_SOL_SOCKET, NN_IPV4ONLY, &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    host = nn_chunkref_data (&self->remote_host);
    nn_assert (strlen (host) > 0);

    nn_dns_start (&self->dns, host, self->remote_hostname_len, ipv4only,
        &self->dns_result);

    self->state = NN_CWS_STATE_RESOLVING;
}

static void nn_cws_start_connect (struct nn_cstream *cstream)
{
    int rc;
    struct sockaddr_storage remote;
    size_t remotelen;
    struct sockaddr_storage local;
    size_t locallen;
    int ipv4only;
    size_t ipv4onlylen;
    int val;
    size_t sz;

    memset (&remote, 0, sizeof (remote));
    memset (&local, 0, sizeof (local));

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    nn_assert (ipv4onlylen == sizeof (ipv4only));

    rc = nn_iface_resolve (nn_chunkref_data (&self->nic),
        nn_chunkref_size (&self->nic), ipv4only, &local, &locallen);

    if (rc < 0) {
        nn_backoff_start (&self->retry, NN_STREAM_CONNECT_TIMEDOUT);
        self->state = NN_CWS_STATE_WAITING;
        return;
    }

    /*  Combine the remote address and the port. */
    remote = *ss;
    remotelen = sslen;
    if (remote.ss_family == AF_INET)
        ((struct sockaddr_in*) &remote)->sin_port = htons (self->remote_port);
    else if (remote.ss_family == AF_INET6)
        ((struct sockaddr_in6*) &remote)->sin6_port = htons (self->remote_port);
    else {
        nn_assert_unreachable ("Unexpected ss_family.");
    }

    /*  Try to start the underlying socket. */
    rc = nn_utcp_start (&self->usock, remote.ss_family, SOCK_STREAM, 0);
    if (rc < 0) {
        nn_backoff_start (&self->retry, NN_STREAM_CONNECT_TIMEDOUT);
        self->state = NN_CWS_STATE_WAITING;
        return;
    }

    /*  Set the relevant socket options. */
    sz = sizeof (val);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_SNDBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_utcp_setsockopt (&self->usock, SOL_SOCKET, SO_SNDBUF,
        &val, sizeof (val));
    sz = sizeof (val);
    nn_epbase_getopt (&self->epbase, NN_SOL_SOCKET, NN_RCVBUF, &val, &sz);
    nn_assert (sz == sizeof (val));
    nn_utcp_setsockopt (&self->usock, SOL_SOCKET, SO_RCVBUF,
        &val, sizeof (val));

    /*  Bind the socket to the local network interface. */
    rc = nn_utcp_bind (&self->usock, (struct sockaddr*) &local, locallen);
    if (rc != 0) {
        nn_backoff_start (&self->retry, NN_STREAM_CONNECT_TIMEDOUT);
        self->state = NN_CWS_STATE_WAITING;
        return;
    }

    /*  Start connecting. */
    nn_utcp_connect (&self->usock, (struct sockaddr*) &remote, remotelen);
}

int nn_ws_tune (struct nn_stream *s, struct nn_epbase *e)
{
    struct nn_sws *sws = nn_cont (s, struct nn_sws, stream);
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

    optsz = sizeof (opt);
    nn_epbase_getopt (e, NN_WS, NN_WS_MSG_TYPE, &opt, &optsz);
    nn_assert (optsz == sizeof (opt));
    sws->msg_type = (uint8_t) opt;

    /*  Since the WebSocket handshake must poll, the receive
        timeout is set to zero. Later, it will be set again
        to the value specified by the socket option. */
    opt = 0;
    optsz = sizeof (opt);
    nn_stream_setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &opt, optsz);

    return 0;
}

static int nn_ws_activate (struct nn_astream *as)
{
    as->usock

    nn_sws_start (&aws->sws, &aws->usock, NN_WS_SERVER,
        NULL, NULL, msg_type);

    return 0;
}

static int nn_ws_start_listen (struct nn_stream *s, struct nn_epbase *e)
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
    nn_assert (pos);
    ++pos;
    rc = nn_port_resolve (pos, end - pos);
    if (rc < 0) {
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

    rc = nn_utcp_listen (s, NN_BWS_BACKLOG);
    if (rc < 0) {
        nn_utcp_stop (s);
        return rc;
    }

    return 0;
}