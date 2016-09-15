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

#include "astream.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#define NN_ASTREAM_STATE_IDLE 1
#define NN_ASTREAM_STATE_ACCEPTING 2
#define NN_ASTREAM_STATE_ACTIVE 3
#define NN_ASTREAM_STATE_STOPPING_SSTREAM 4
#define NN_ASTREAM_STATE_STOPPING_USOCK 5
#define NN_ASTREAM_STATE_DONE 6
#define NN_ASTREAM_STATE_STOPPING_SSTREAM_FINAL 7
#define NN_ASTREAM_STATE_STOPPING 8

/*  Private functions. */
static void nn_astream_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_astream_shutdown (struct nn_fsm *self, int type, void *srcptr);

void nn_astream_init (struct nn_astream *self, struct nn_epbase *epbase,
    struct nn_fsm *owner, struct nn_stream_vfptr *vft)
{
    nn_fsm_init (&self->fsm, nn_astream_handler, nn_astream_shutdown,
        self, owner);
    self->state = NN_ASTREAM_STATE_IDLE;
    self->epbase = epbase;
    self->vft = vft;
    nn_stream_init (&self->usock, &self->fsm, self->vft);
    self->listener = NULL;
    self->owner = NULL;
    nn_sstream_init (&self->sstream, epbase, &self->fsm);
    nn_fsm_event_init (&self->accepted);
    nn_fsm_event_init (&self->done);
    nn_list_item_init (&self->item);
}

void nn_astream_term (struct nn_astream *self)
{
    nn_assert_state (self, NN_ASTREAM_STATE_IDLE);

    nn_list_item_term (&self->item);
    nn_fsm_event_term (&self->done);
    nn_fsm_event_term (&self->accepted);
    nn_sstream_term (&self->sstream);
    nn_stream_term (&self->usock);
    nn_fsm_term (&self->fsm);
}

int nn_astream_isidle (struct nn_astream *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_astream_start (struct nn_astream *self, struct nn_stream *listener)
{
    nn_assert_state (self, NN_ASTREAM_STATE_IDLE);

    /*  Take ownership of the listener socket. */
    self->listener = listener;
    nn_stream_swap_owner (listener, &self->fsm);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);
}

void nn_astream_stop (struct nn_astream *self)
{
    nn_fsm_stop (&self->fsm);
}

static void nn_astream_shutdown (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_astream *astream;

    astream = nn_cont (self, struct nn_astream, fsm);

    if (type == NN_FSM_STOP) {
        if (!nn_sstream_isidle (&astream->sstream)) {
            nn_epbase_stat_increment (astream->epbase,
                NN_STAT_DROPPED_CONNECTIONS, 1);
            nn_sstream_stop (&astream->sstream);
        }
        astream->state = NN_ASTREAM_STATE_STOPPING_SSTREAM_FINAL;
    }
    if (astream->state == NN_ASTREAM_STATE_STOPPING_SSTREAM_FINAL) {
        if (!nn_sstream_isidle (&astream->sstream))
            return;
        nn_stream_stop (&astream->usock);
        astream->state = NN_ASTREAM_STATE_STOPPING;
    }
    if (astream->state == NN_ASTREAM_STATE_STOPPING) {
        if (!nn_stream_isidle (&astream->usock))
            return;
       if (astream->listener) {
            nn_assert (astream->owner);
            nn_stream_swap_owner (astream->listener, astream->owner);
            astream->listener = NULL;
            astream->owner = NULL;
        }
        astream->state = NN_ASTREAM_STATE_IDLE;
        nn_fsm_stopped (&astream->fsm, NN_ASTREAM_STOPPED);
        return;
    }

    nn_assert_unreachable_fsm (astream->state, type);
}

static void nn_astream_handler (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_astream *astream;
    int rc;

    astream = nn_cont (self, struct nn_astream, fsm);

    switch (astream->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case NN_ASTREAM_STATE_IDLE:
        
            switch (type) {
            case NN_FSM_START:
                nn_assert (srcptr == NULL);
                nn_stream_accept (&astream->usock, astream->listener);
                astream->state = NN_ASTREAM_STATE_ACCEPTING;
                return;
            default:
                nn_assert_unreachable_fsm (astream->state, type);
                return;
            }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  Waiting for incoming connection.                                          */
/******************************************************************************/
    case NN_ASTREAM_STATE_ACCEPTING:
        switch (type) {
        case NN_STREAM_ACCEPTED:
            nn_assert (srcptr == &astream->usock);
            nn_epbase_clear_error (astream->epbase);

            rc = astream->vft->tune (&astream->usock, astream->epbase);
            nn_assert (rc == 0);

            /*  Return ownership of the listening socket to the parent. */
            nn_stream_swap_owner (astream->listener, astream->owner);
            astream->listener = NULL;
            astream->owner = NULL;
            nn_fsm_raise (&astream->fsm, &astream->accepted, NN_ASTREAM_ACCEPTED);

            /*  Start the sstream state machine. */
            rc = astream->vft->activate (astream);
            nn_assert (rc == 0);
            nn_fsm_do_now (&astream->usock.fsm, NN_STREAM_ACTIVATE);
            nn_sstream_start (&astream->sstream, &astream->usock);
            astream->state = NN_ASTREAM_STATE_ACTIVE;

            nn_epbase_stat_increment (astream->epbase,
                NN_STAT_ACCEPTED_CONNECTIONS, 1);

            return;

        case NN_STREAM_ACCEPT_ERROR:
            nn_assert (srcptr == &astream->usock);
            nn_epbase_set_error (astream->epbase, astream->listener->err);
            nn_epbase_stat_increment (astream->epbase,
                NN_STAT_ACCEPT_ERRORS, 1);
            nn_stream_accept (&astream->usock, astream->listener);

            return;

        default:
            nn_assert_unreachable_fsm (astream->state, type);
            return;
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case NN_ASTREAM_STATE_ACTIVE:
        switch (type) {
        case NN_SSTREAM_ERROR:
            nn_assert (srcptr == &astream->usock);
            nn_sstream_stop (&astream->sstream);
            astream->state = NN_ASTREAM_STATE_STOPPING_SSTREAM;
            nn_epbase_stat_increment (astream->epbase,
                NN_STAT_BROKEN_CONNECTIONS, 1);
            return;
        default:
            nn_assert_unreachable_fsm (astream->state, type);
            return;
        }

/******************************************************************************/
/*  STOPPING_SSTREAM state.                                                      */
/******************************************************************************/
    case NN_ASTREAM_STATE_STOPPING_SSTREAM:
        switch (type) {
        case NN_STREAM_SHUTDOWN:
            nn_assert (srcptr == &astream->usock);
            return;
        case NN_SSTREAM_STOPPED:
            nn_assert (srcptr == &astream->usock);
            nn_stream_stop (&astream->usock);
            astream->state = NN_ASTREAM_STATE_STOPPING_USOCK;
            return;
        default:
            nn_assert_unreachable_fsm (astream->state, type);
            return;
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                      */
/******************************************************************************/
    case NN_ASTREAM_STATE_STOPPING_USOCK:
        switch (type) {
        case NN_STREAM_SHUTDOWN:
            nn_assert (srcptr == &astream->usock);
            return;
        case NN_STREAM_STOPPED:
            nn_assert (srcptr == &astream->usock);
            nn_fsm_raise (&astream->fsm, &astream->done, NN_ASTREAM_ERROR);
            astream->state = NN_ASTREAM_STATE_DONE;
            return;
        default:
            nn_assert_unreachable_fsm (astream->state, type);
            return;
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_assert_unreachable_fsm (astream->state, type);
        return;
    }
}
