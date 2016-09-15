/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>
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

#include "bstream.h"
#include "astream.h"
#include "ustream.h"

#include "../../aio/fsm.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"

#include <string.h>

#if defined NN_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>
#endif

#define NN_BSTREAM_STATE_IDLE 1
#define NN_BSTREAM_STATE_ACTIVE 2
#define NN_BSTREAM_STATE_STOPPING_ASTREAM 3
#define NN_BSTREAM_STATE_STOPPING_USOCK 4
#define NN_BSTREAM_STATE_STOPPING_ASTREAMS 5

/*  nn_epbase virtual interface implementation. */
static void nn_bstream_stop (struct nn_epbase *self);
static void nn_bstream_destroy (struct nn_epbase *self);
const struct nn_epbase_vfptr nn_bstream_epbase_vfptr = {
    nn_bstream_stop,
    nn_bstream_destroy
};

/*  Private functions. */
static void nn_bstream_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_bstream_shutdown (struct nn_fsm *self, int type, void *srcptr);
static int nn_bstream_listen (struct nn_bstream *self);
static void nn_bstream_start_accept (struct nn_bstream *self);

int nn_bstream_create (struct nn_bstream *bstream, void *hint,
    struct nn_epbase **epbase, struct nn_stream_vfptr *vft)
{
    int rc;

    /*  Initialise the structure. */
    nn_epbase_init (&self->epbase, &nn_bstream_epbase_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_bstream_handler, nn_bstream_shutdown,
        nn_epbase_getctx (&self->epbase));
    self->vft = vft;
    self->state = NN_BSTREAM_STATE_IDLE;
    self->astream = NULL;
    nn_list_init (&self->astreams);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    nn_stream_init (&self->usock, &self->fsm, vft);

    rc = vft->start_listen (&self->usock, &self->epbase);
    if (rc != 0) {
        nn_epbase_term (&self->epbase);
        return rc;
    }
    
    nn_bstream_start_accept (self);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void nn_bstream_stop (struct nn_epbase *self)
{
    struct nn_bstream *bstream;

    bstream = nn_cont (self, struct nn_bstream, epbase);

    nn_fsm_stop (&bstream->fsm);
}

static void nn_bstream_destroy (struct nn_epbase *self)
{
    struct nn_bstream *bstream;

    bstream = nn_cont (self, struct nn_bstream, epbase);

    nn_assert_state (bstream, NN_BSTREAM_STATE_IDLE);
    nn_list_term (&bstream->astreams);
    nn_assert (bstream->astream == NULL);
    nn_stream_term (&bstream->usock);
    nn_epbase_term (&bstream->epbase);
    nn_fsm_term (&bstream->fsm);

    nn_free (bstream);
}

static void nn_bstream_shutdown (struct nn_fsm *self, int type, void *srcptr)
{
#if defined NN_HAVE_UNIX_SOCKETS
    const char *addr;
    int rc;
#endif

    struct nn_bstream *bstream;
    struct nn_list_item *it;
    struct nn_astream *astream;

    bstream = nn_cont (self, struct nn_bstream, fsm);
    nn_assert (srcptr == NULL);

    if (type == NN_FSM_STOP) {
        if (bstream->astream) {
            nn_astream_stop (bstream->astream);
            bstream->state = NN_BSTREAM_STATE_STOPPING_ASTREAM;
        }
        else {
            bstream->state = NN_BSTREAM_STATE_STOPPING_USOCK;
        }
    }
    if (bstream->state == NN_BSTREAM_STATE_STOPPING_ASTREAM) {
        if (!nn_astream_isidle (bstream->astream))
            return;
        nn_astream_term (bstream->astream);
        nn_free (bstream->astream);
        bstream->astream = NULL;

        /* On *nixes, unlink the domain socket file */
#if defined NN_HAVE_UNIX_SOCKETS
        addr = nn_epbase_getaddr (&bstream->epbase);
        rc = unlink(addr);
        errno_assert (rc == 0 || errno == ENOENT);
#endif

        nn_stream_stop (&bstream->usock);
        bstream->state = NN_BSTREAM_STATE_STOPPING_USOCK;
    }
    if (bstream->state == NN_BSTREAM_STATE_STOPPING_USOCK) {
       if (!nn_stream_isidle (&bstream->usock))
            return;
        for (it = nn_list_begin (&bstream->astreams);
              it != nn_list_end (&bstream->astreams);
              it = nn_list_next (&bstream->astreams, it)) {
            astream = nn_cont (it, struct nn_astream, item);
            nn_astream_stop (astream);
        }
        bstream->state = NN_BSTREAM_STATE_STOPPING_ASTREAMS;
        goto astreams_stopping;
    }
    if (bstream->state == NN_BSTREAM_STATE_STOPPING_ASTREAMS) {
        nn_assert (type == NN_ASTREAM_STOPPED);
        astream = (struct nn_astream *) srcptr;
        nn_list_erase (&bstream->astreams, &astream->item);
        nn_astream_term (astream);
        nn_free (astream);

        /*  If there are no more astream state machines, we can stop the whole
            bstream object. */
astreams_stopping:
        if (nn_list_empty (&bstream->astreams)) {
            bstream->state = NN_BSTREAM_STATE_IDLE;
            nn_fsm_stopped_noevent (&bstream->fsm);
            nn_epbase_stopped (&bstream->epbase);
            return;
        }

        return;
    }

    nn_assert_unreachable_fsm (bstream->state, type);
}

static void nn_bstream_handler (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_bstream *bstream = nn_cont (self, struct nn_bstream, fsm);
    struct nn_astream *astream;

    switch (bstream->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_BSTREAM_STATE_IDLE:
        nn_assert (type == NN_FSM_START);
        nn_assert (srcptr == NULL);
        bstream->state = NN_BSTREAM_STATE_ACTIVE;
        return;

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the astream state machine in this state.         */
/******************************************************************************/
    case NN_BSTREAM_STATE_ACTIVE:
        switch (type) {
        case NN_STREAM_SHUTDOWN:
        case NN_STREAM_STOPPED:
            nn_assert (srcptr == &bstream->usock);
            return;
        case NN_ASTREAM_ACCEPTED:
            nn_assert (srcptr == bstream->astream);
            astream = (struct nn_astream*) srcptr;
            nn_list_insert_at_end (&bstream->astreams, &astream->item);
            bstream->astream = NULL;
            nn_bstream_start_accept (bstream);
            return;
        case NN_ASTREAM_ERROR:
            nn_assert (srcptr == bstream->astream);
            astream = (struct nn_astream*) srcptr;
            nn_astream_stop (astream);
            return;
        case NN_ASTREAM_STOPPED:
            nn_assert (srcptr == bstream->astream);
            astream = (struct nn_astream*) srcptr;
            nn_list_erase (&bstream->astreams, &astream->item);
            nn_astream_term (astream);
            nn_free (astream);
            return;
        default:
            nn_assert_unreachable_fsm (bstream->state, type);
            return;
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_assert_unreachable_fsm (bstream->state, type);
        return;
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void nn_bstream_start_accept (struct nn_bstream *self)
{
    nn_assert (self->astream == NULL);

    /*  Allocate new astream state machine. */
    self->astream = nn_alloc (sizeof (struct nn_astream), "astream");
    nn_assert_alloc (self->astream);
    nn_astream_init (self->astream, &self->epbase, &self->fsm, self->vft);

    /*  Start waiting for a new incoming connection. */
    nn_astream_start (self->astream, &self->usock);
}
