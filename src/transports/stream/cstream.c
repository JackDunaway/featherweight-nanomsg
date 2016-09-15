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

#include "cstream.h"

#include "../../aio/fsm.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/attr.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#endif

#define NN_CSTREAM_STATE_IDLE 1
#define NN_CSTREAM_STATE_RESOLVING_NAME 2
#define NN_CSTREAM_STATE_CONNECTING 4
#define NN_CSTREAM_STATE_ACTIVE 5
#define NN_CSTREAM_STATE_STOPPING_SSTREAM 6
#define NN_CSTREAM_STATE_STOPPING_USOCK 7
#define NN_CSTREAM_STATE_WAITING 8
#define NN_CSTREAM_STATE_STOPPING_BACKOFF 9
#define NN_CSTREAM_STATE_STOPPING_SSTREAM_FINAL 10
#define NN_CSTREAM_STATE_STOPPING 11

#define NN_RESOLVER_SUCCESS 1
#define NN_RESOLVER_ERROR 2

/*  nn_epbase virtual interface implementation. */
static void nn_cstream_stop (struct nn_epbase *self);
static void nn_cstream_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_cstream_epbase_vfptr = {
    nn_cstream_stop,
    nn_cstream_destroy
};

/*  Private functions. */
static void nn_cstream_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_cstream_shutdown (struct nn_fsm *self, int type, void *srcptr);

int nn_cstream_create (struct nn_cstream *self, void *hint,
    struct nn_epbase **epbase, struct nn_stream_vfptr *vft)
{
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Initialise the structure. */
    nn_epbase_init (&self->epbase, &nn_cstream_epbase_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_cstream_handler, nn_cstream_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->state = NN_CSTREAM_STATE_IDLE;
    self->vft = vft;
    nn_stream_init (&self->usock, &self->fsm, vft);
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
    nn_backoff_init (&self->retry, self->usock.worker, reconnect_ivl, reconnect_ivl_max,
        &self->fsm);

    nn_sstream_init (&self->sstream, &self->epbase, &self->fsm);
    self->vft->start_resolve (self);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_cstream_stop (struct nn_epbase *self)
{
    struct nn_cstream *cstream;

    cstream = nn_cont (self, struct nn_cstream, epbase);

    nn_fsm_stop (&cstream->fsm);
}

static void nn_cstream_destroy (struct nn_epbase *self)
{
    struct nn_cstream *cstream;

    cstream = nn_cont (self, struct nn_cstream, epbase);

    nn_sstream_term (&cstream->sstream);
    nn_backoff_term (&cstream->retry);
    nn_stream_term (&cstream->usock);
    nn_fsm_term (&cstream->fsm);
    nn_epbase_term (&cstream->epbase);

    nn_free (cstream);
}

static void nn_cstream_shutdown (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_cstream *cstream;

    cstream = nn_cont (self, struct nn_cstream, fsm);
    nn_assert (srcptr == NULL);

    if (type == NN_FSM_STOP) {
        if (!nn_sstream_isidle (&cstream->sstream)) {
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_DROPPED_CONNECTIONS, 1);
            nn_sstream_stop (&cstream->sstream);
        }
        cstream->state = NN_CSTREAM_STATE_STOPPING_SSTREAM_FINAL;
    }
    if (cstream->state == NN_CSTREAM_STATE_STOPPING_SSTREAM_FINAL) {
        if (!nn_sstream_isidle (&cstream->sstream))
            return;
        nn_backoff_cancel (&cstream->retry);
        nn_stream_stop (&cstream->usock);
        cstream->state = NN_CSTREAM_STATE_STOPPING;
    }
    if (cstream->state == NN_CSTREAM_STATE_STOPPING) {
        if (!nn_backoff_isidle (&cstream->retry) ||
              !nn_stream_isidle (&cstream->usock))
            return;
        cstream->state = NN_CSTREAM_STATE_IDLE;
        nn_fsm_stopped_noevent (&cstream->fsm);
        nn_epbase_stopped (&cstream->epbase);
        return;
    }

    nn_assert_unreachable_fsm (cstream->state, type);
}

static void nn_cstream_handler (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_cstream *cstream;
    int rc;

    cstream = nn_cont (self, struct nn_cstream, fsm);

    switch (cstream->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case NN_CSTREAM_STATE_IDLE:
        switch (type) {
        case NN_FSM_START:
            nn_assert (srcptr == NULL);
            rc = cstream->vft->start_resolve (cstream);
            switch (rc) {
            case 1:
                /*  Async name resolution has started. */
                self->state = NN_CSTREAM_STATE_RESOLVING_NAME;
                return;
            default:
                nn_assert_unreachable ("Unexpected resolver return code.");
                return;
            }
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  RESOLVING state.                                                          */
/*  Name of the host to connect to is being resolved to get an IP address.    */
/******************************************************************************/
    case NN_CSTREAM_STATE_RESOLVING_NAME:
        switch (type) {
        case NN_RESOLVER_SUCCESS:
            rc = cstream->vft->start_connect (cstream);
            /*  Async connection operation has begun. */
            if (rc == 0) {
                self->state = NN_CSTREAM_STATE_CONNECTING;
                nn_epbase_stat_increment (&cstream->epbase, NN_STAT_INPROGRESS_CONNECTIONS, 1);
                return;
            }
            /*  TODO: how to handle failure? */
            /*  Failed to begin async connection. */
            nn_assert (rc == -1);
            return;

        case NN_RESOLVER_ERROR:
            nn_backoff_start (&cstream->retry, NN_STREAM_CONNECT_TIMEDOUT);
            cstream->state = NN_CSTREAM_STATE_WAITING;
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Non-blocking connect is under way.                                        */
/******************************************************************************/
    case NN_CSTREAM_STATE_CONNECTING:
        switch (type) {
        case NN_STREAM_CONNECTED:
            nn_assert (srcptr == &cstream->usock);
            nn_sstream_start (&cstream->sstream, &cstream->usock);
            cstream->state = NN_CSTREAM_STATE_ACTIVE;
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_INPROGRESS_CONNECTIONS, -1);
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_ESTABLISHED_CONNECTIONS, 1);
            nn_epbase_clear_error (&cstream->epbase);
            return;
        case NN_STREAM_ERROR:
            nn_assert (srcptr == &cstream->usock);
            nn_epbase_set_error (&cstream->epbase, cstream->usock.err);
            nn_stream_stop (&cstream->usock);
            cstream->state = NN_CSTREAM_STATE_STOPPING_USOCK;
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_INPROGRESS_CONNECTIONS, -1);
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_CONNECT_ERRORS, 1);
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the sstream state machine.          */
/******************************************************************************/
    case NN_CSTREAM_STATE_ACTIVE:
        switch (type) {
        case NN_SSTREAM_ERROR:
            nn_assert (srcptr == &cstream->usock);
            nn_sstream_stop (&cstream->sstream);
            cstream->state = NN_CSTREAM_STATE_STOPPING_SSTREAM;
            nn_epbase_stat_increment (&cstream->epbase,
                NN_STAT_BROKEN_CONNECTIONS, 1);
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  STOPPING_SSTREAM state.                                                      */
/*  sstream object was asked to stop but it haven't stopped yet.                 */
/******************************************************************************/
    case NN_CSTREAM_STATE_STOPPING_SSTREAM:
        switch (type) {
        case NN_STREAM_SHUTDOWN:
            nn_assert (srcptr == &cstream->usock);
            return;
        case NN_SSTREAM_STOPPED:
            nn_assert (srcptr == &cstream->usock);
            nn_stream_stop (&cstream->usock);
            cstream->state = NN_CSTREAM_STATE_STOPPING_USOCK;
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case NN_CSTREAM_STATE_STOPPING_USOCK:
        switch (type) {
        case NN_STREAM_SHUTDOWN:
            nn_assert (srcptr == &cstream->usock);
            return;
        case NN_STREAM_STOPPED:
            nn_assert (srcptr == &cstream->usock);
            if (cstream->persistent) {
                nn_backoff_start (&cstream->retry, NN_STREAM_CONNECT_TIMEDOUT);
                cstream->state = NN_CSTREAM_STATE_WAITING;
            }
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we won't overload     */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case NN_CSTREAM_STATE_WAITING:
        switch (type) {
        case NN_STREAM_CONNECT_TIMEDOUT:
            nn_assert (srcptr == &cstream->usock);
            nn_backoff_cancel (&cstream->retry);
            cstream->state = NN_CSTREAM_STATE_STOPPING_BACKOFF;
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case NN_CSTREAM_STATE_STOPPING_BACKOFF:
        switch (type) {
        case NN_EVENT_TIMER_STOPPED:
            nn_assert (srcptr == &cstream->retry);
            cstream->vft->start_resolve (cstream);
            return;
        default:
            nn_assert_unreachable_fsm (cstream->state, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_assert_unreachable_fsm (cstream->state, type);
    }
}
