/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.

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

#include "dns.h"

#include "../../utils/err.h"

#include <string.h>

int nn_dns_check_hostname (const char *name, size_t namelen)
{
    int labelsz;

    /*  There has to be at least one label in the hostname.
        Additionally, hostnames are up to 255 characters long. */
    if (namelen < 1 || namelen > 255)
        return -EINVAL;

    /*  Hyphen can't be used as a first character of the hostname. */
    if (*name == '-')
        return -EINVAL;

    labelsz = 0;
    while (1) {

        /*  End of the hostname. */
        if (namelen == 0) {

            /*  The last label cannot be empty. */
            if (labelsz == 0)
                return -EINVAL;

            /*  Success! */
            return 0;
        }

        /*  End of a label. */
        if (*name == '.') {

            /*  The old label cannot be empty. */
            if (labelsz == 0)
                return -EINVAL;

            /*  Start new label. */
            labelsz = 0;
            ++name;
            --namelen;
            continue;
        }

        /*  Valid character. */
        if ((*name >= 'a' && *name <= 'z') ||
              (*name >= 'A' && *name <= 'Z') ||
              (*name >= '0' && *name <= '9') ||
              *name == '-') {
            ++name;
            --namelen;
            ++labelsz;

            /*  Labels longer than 63 charcters are not permitted. */
            if (labelsz > 63)
                return -EINVAL;

            continue;
        }

        /*  Invalid character. */
        return -EINVAL;
    }
}

#if defined NN_HAVE_GETADDRINFO_A && !defined NN_DISABLE_GETADDRINFO_A

#include "literal.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#include "../../aio/ctx.h"

#include <signal.h>

#define NN_DNS_STATE_IDLE 1
#define NN_DNS_STATE_RESOLVING 2
#define NN_DNS_STATE_DONE 3
#define NN_DNS_STATE_STOPPING 4

#define NN_DNS_ACTION_DONE 1
#define NN_DNS_ACTION_CANCELLED 2

/*  Private functions. */
static void nn_dns_notify (union sigval);
static void nn_dns_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_dns_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_dns_init (struct nn_dns *self, int src, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_dns_handler, nn_dns_shutdown, src, self, owner);
    self->state = NN_DNS_STATE_IDLE;
    nn_fsm_event_init (&self->done);
}

void nn_dns_term (struct nn_dns *self)
{
    nn_assert_state (self, NN_DNS_STATE_IDLE);

    nn_fsm_event_term (&self->done);
    nn_fsm_term (&self->fsm);
}

int nn_dns_isidle (struct nn_dns *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_dns_start (struct nn_dns *self, const char *addr, size_t addrlen,
    int ipv4only, struct nn_dns_result *result)
{
    int rc;
    struct gaicb *pgcb;
    struct sigevent sev;

    nn_assert_state (self, NN_DNS_STATE_IDLE);

    self->result = result;

    /*  Try to resolve the supplied string as a literal address. In this case,
        there's no DNS lookup involved. */
    rc = nn_literal_resolve (addr, addrlen, ipv4only, &self->result->addr,
        &self->result->addrlen);
    if (rc == 0) {
        self->result->error = 0;
        nn_fsm_start (&self->fsm);
        return;
    }
    errnum_assert (rc == -EINVAL, -rc);

    /*  Make a zero-terminated copy of the address string. */
    nn_assert (sizeof (self->hostname) > addrlen);
    memcpy (self->hostname, addr, addrlen);
    self->hostname [addrlen] = 0;

    /*  Start asynchronous DNS lookup. */
    memset (&self->request, 0, sizeof (self->request));
    if (ipv4only)
        self->request.ai_family = AF_INET;
    else {
        self->request.ai_family = AF_INET6;
#ifdef AI_V4MAPPED
        self->request.ai_flags = AI_V4MAPPED;
#endif
    }
    self->request.ai_socktype = SOCK_STREAM;

    memset (&self->gcb, 0, sizeof (self->gcb));
    self->gcb.ar_name = self->hostname;
    self->gcb.ar_service = NULL;
    self->gcb.ar_request = &self->request;
    self->gcb.ar_result = NULL;
    pgcb = &self->gcb;

    memset (&sev, 0, sizeof (sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = nn_dns_notify;
    sev.sigev_value.sival_ptr = self;

    rc = getaddrinfo_a (GAI_NOWAIT, &pgcb, 1, &sev);
    nn_assert (rc == 0);

    self->result->error = EINPROGRESS;
    nn_fsm_start (&self->fsm);
}

void nn_dns_stop (struct nn_dns *self)
{
    nn_fsm_stop (&self->fsm);
}

static void nn_dns_notify (union sigval sval)
{
    int rc;
    struct nn_dns *self;

    self = (struct nn_dns*) sval.sival_ptr;

    nn_ctx_enter (self->fsm.ctx);
    rc = gai_error (&self->gcb);
    if (rc == EAI_CANCELED) {
        nn_fsm_action (&self->fsm, NN_DNS_ACTION_CANCELLED);
    }
    else if (rc != 0) {
        self->result->error = EINVAL;
        nn_fsm_action (&self->fsm, NN_DNS_ACTION_DONE);
    }
    else {
        self->result->error = 0;
        nn_assert (self->gcb.ar_result->ai_addrlen <=
            sizeof (struct sockaddr_storage));
        memcpy (&self->result->addr, self->gcb.ar_result->ai_addr,
            self->gcb.ar_result->ai_addrlen);
        self->result->addrlen = (size_t) self->gcb.ar_result->ai_addrlen;
        freeaddrinfo(self->gcb.ar_result);
        nn_fsm_action (&self->fsm, NN_DNS_ACTION_DONE);
    }
    nn_ctx_leave (self->fsm.ctx);
}

static void nn_dns_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    int rc;
    struct nn_dns *dns;

    dns = nn_cont (self, struct nn_dns, fsm);

    if (src == NN_FSM_ACTION && type == NN_FSM_STOP) {
        if (dns->state == NN_DNS_STATE_RESOLVING) {
            rc = gai_cancel (&dns->gcb);
            if (rc == EAI_CANCELED) {
                nn_fsm_stopped (&dns->fsm, NN_DNS_STOPPED);
                dns->state = NN_DNS_STATE_IDLE;
                return;
            }
            if (rc == EAI_NOTCANCELED || rc == EAI_ALLDONE) {
                dns->state = NN_DNS_STATE_STOPPING;
                return;
            }
            nn_assert_unreachable ("Unexpected gai_cancel return code.");
        }
        nn_fsm_stopped (&dns->fsm, NN_DNS_STOPPED);
        dns->state = NN_DNS_STATE_IDLE;
        return;
    }
    if (dns->state == NN_DNS_STATE_STOPPING) {
        if (src == NN_FSM_ACTION && (type == NN_DNS_ACTION_CANCELLED ||
            type == NN_DNS_ACTION_DONE)) {
            nn_fsm_stopped (&dns->fsm, NN_DNS_STOPPED);
            dns->state = NN_DNS_STATE_IDLE;
            return;
        }
        return;
    }

    nn_fsm_bad_state (dns->state, src, type);
}

static void nn_dns_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_dns *dns;

    dns = nn_cont (self, struct nn_dns, fsm);

    switch (dns->state) {
/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_DNS_STATE_IDLE:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                if (dns->result->error == EINPROGRESS) {
                    dns->state = NN_DNS_STATE_RESOLVING;
                    return;
                }
                nn_fsm_raise (&dns->fsm, &dns->done, NN_DNS_DONE);
                dns->state = NN_DNS_STATE_DONE;
                return;
            default:
                nn_fsm_bad_action (dns->state, src, type);
            }
        default:
            nn_fsm_bad_source (dns->state, src, type);
        }

/******************************************************************************/
/*  RESOLVING state.                                                          */
/******************************************************************************/
    case NN_DNS_STATE_RESOLVING:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_DNS_ACTION_DONE:
                nn_fsm_raise (&dns->fsm, &dns->done, NN_DNS_DONE);
                dns->state = NN_DNS_STATE_DONE;
                return;
            default:
                nn_fsm_bad_action (dns->state, src, type);
            }
        default:
            nn_fsm_bad_source (dns->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/******************************************************************************/
    case NN_DNS_STATE_DONE:
        nn_fsm_bad_source (dns->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (dns->state, src, type);
    }
}

#else

#include "literal.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#include <string.h>

#ifndef NN_HAVE_WINDOWS
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

#define NN_DNS_STATE_IDLE 1
#define NN_DNS_STATE_DONE 2

/*  Private functions. */
static void nn_dns_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_dns_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_dns_init (struct nn_dns *self, int src, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_dns_handler, nn_dns_shutdown,
        src, self, owner);
    self->state = NN_DNS_STATE_IDLE;
    nn_fsm_event_init (&self->done);
}

void nn_dns_term (struct nn_dns *self)
{
    nn_assert_state (self, NN_DNS_STATE_IDLE);

    nn_fsm_event_term (&self->done);
    nn_fsm_term (&self->fsm);
}

int nn_dns_isidle (struct nn_dns *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_dns_start (struct nn_dns *self, const char *addr, size_t addrlen,
    int ipv4only, struct nn_dns_result *result)
{
    int rc;
    struct addrinfo query;
    struct addrinfo *reply;
    char hostname [NN_SOCKADDR_MAX];

    nn_assert_state (self, NN_DNS_STATE_IDLE);

    self->result = result;

    /*  Try to resolve the supplied string as a literal address. In this case,
        there's no DNS lookup involved. */
    rc = nn_literal_resolve (addr, addrlen, ipv4only, &self->result->addr,
        &self->result->addrlen);
    if (rc == 0) {
        self->result->error = 0;
        nn_fsm_start (&self->fsm);
        return;
    }
    errnum_assert (rc == -EINVAL, -rc);

    /*  The name is not a literal. Let's do an actual DNS lookup. */
    memset (&query, 0, sizeof (query));
    if (ipv4only)
        query.ai_family = AF_INET;
    else {
        query.ai_family = AF_INET6;
#ifdef AI_V4MAPPED
        query.ai_flags = AI_V4MAPPED;
#endif
    }
    nn_assert (sizeof (hostname) > addrlen);
    query.ai_socktype = SOCK_STREAM;
    memcpy (hostname, addr, addrlen);
    hostname [addrlen] = 0;

    /*  Perform the DNS lookup itself. */
    self->result->error = getaddrinfo (hostname, NULL, &query, &reply);
    if (self->result->error) {
        nn_fsm_start (&self->fsm);
        return;
    }

    /*  Take just the first address and store it. (RFC recommends that we
        iterate through addresses until one works, but that doesn't match
        our state model. This is the best we can do.) */
    self->result->error = 0;
    memcpy (&self->result->addr, reply->ai_addr, reply->ai_addrlen);
    self->result->addrlen = reply->ai_addrlen;
    freeaddrinfo (reply);

    nn_fsm_start (&self->fsm);
}

void nn_dns_stop (struct nn_dns *self)
{
    nn_fsm_stop (&self->fsm);
}

static void nn_dns_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_dns *dns;

    dns = nn_cont (self, struct nn_dns, fsm);
    if (src == NN_FSM_ACTION && type == NN_FSM_STOP) {
        nn_fsm_stopped (&dns->fsm, NN_DNS_STOPPED);
        dns->state = NN_DNS_STATE_IDLE;
        return;
    }

    nn_fsm_bad_state(dns->state, src, type);
}

static void nn_dns_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_dns *dns;

    dns = nn_cont (self, struct nn_dns, fsm);

    switch (dns->state) {
/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_DNS_STATE_IDLE:
        switch (src) {
        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_fsm_raise (&dns->fsm, &dns->done, NN_DNS_DONE);
                dns->state = NN_DNS_STATE_DONE;
                return;
            default:
                nn_fsm_bad_action (dns->state, src, type);
            }
        default:
            nn_fsm_bad_source (dns->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/******************************************************************************/
    case NN_DNS_STATE_DONE:
        nn_fsm_bad_source (dns->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (dns->state, src, type);
    }
}
#endif
