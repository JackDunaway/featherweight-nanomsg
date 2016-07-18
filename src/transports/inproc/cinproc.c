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

#include "cinproc.h"
#include "binproc.h"
#include "ins.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/attr.h"

#include <stddef.h>

#define NN_CINPROC_STATE_IDLE 1
#define NN_CINPROC_STATE_DISCONNECTED 2
#define NN_CINPROC_STATE_ACTIVE 3
#define NN_CINPROC_STATE_STOPPING 4

#define NN_CINPROC_ACTION_CONNECT 1

#define NN_CINPROC_SRC_SINPROC 1

/*  Implementation of nn_epbase callback interface. */
static void nn_cinproc_stop (struct nn_epbase *self);
static void nn_cinproc_destroy (struct nn_epbase *self);
static const struct nn_epbase_vfptr nn_cinproc_vfptr = {
    nn_cinproc_stop,
    nn_cinproc_destroy
};

/*  Private functions. */
static void nn_cinproc_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_cinproc_connect (struct nn_ins_item *self,
    struct nn_ins_item *peer);

int nn_cinproc_create (void *hint, struct nn_epbase **epbase)
{
    struct nn_cinproc *self;

    self = nn_alloc (sizeof (struct nn_cinproc), "cinproc");
    nn_assert_alloc (self);

    nn_ins_item_init (&self->item, &nn_cinproc_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_cinproc_handler, nn_cinproc_handler,
        nn_epbase_getctx (&self->item.epbase));
    self->state = NN_CINPROC_STATE_IDLE;
    nn_sinproc_init (&self->sinproc, NN_CINPROC_SRC_SINPROC,
        &self->item.epbase, &self->fsm);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Register the inproc endpoint into a global repository. */
    nn_ins_connect (&self->item, nn_cinproc_connect);

    *epbase = &self->item.epbase;
    return 0;
}

static void nn_cinproc_stop (struct nn_epbase *self)
{
    struct nn_cinproc *cinproc;

    cinproc = nn_cont (self, struct nn_cinproc, item.epbase);

    nn_fsm_stop (&cinproc->fsm);
}

static void nn_cinproc_destroy (struct nn_epbase *self)
{
    struct nn_cinproc *cinproc;

    cinproc = nn_cont (self, struct nn_cinproc, item.epbase);

    nn_sinproc_term (&cinproc->sinproc);
    nn_fsm_term (&cinproc->fsm);
    nn_ins_item_term (&cinproc->item);

    nn_free (cinproc);
}

static void nn_cinproc_connect (struct nn_ins_item *self,
    struct nn_ins_item *peer)
{
    struct nn_cinproc *cinproc;
    struct nn_binproc *binproc;

    cinproc = nn_cont (self, struct nn_cinproc, item);
    binproc = nn_cont (peer, struct nn_binproc, item);

    nn_assert_state (cinproc, NN_CINPROC_STATE_DISCONNECTED);
    nn_sinproc_connect (&cinproc->sinproc, &binproc->fsm);
    nn_fsm_action (&cinproc->fsm, NN_CINPROC_ACTION_CONNECT);
}

static void nn_cinproc_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_cinproc *cinproc;

    cinproc = nn_cont (self, struct nn_cinproc, fsm);


    nn_fsm_bad_state(cinproc->state, src, type);
}

static void nn_cinproc_handler (struct nn_fsm *myfsm, int src, int type,
    void *srcptr)
{
    struct nn_cinproc *self = nn_cont (myfsm, struct nn_cinproc, fsm);
    struct nn_epbase *epb = &self->item.epbase;

    NN_FSM_JOB (NN_CINPROC_STATE_IDLE, NN_FSM_ACTION, NN_FSM_START) {
        self->state = NN_CINPROC_STATE_DISCONNECTED;
        nn_epbase_stat_increment (epb, NN_STAT_INPROGRESS_CONNECTIONS, 1);
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_DISCONNECTED, NN_FSM_ACTION, NN_FSM_STOP) {

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        nn_ins_disconnect (&self->item);

        nn_assert (nn_sinproc_isidle (&self->sinproc));
        self->state = NN_CINPROC_STATE_IDLE;
        nn_fsm_stopped_noevent (&self->fsm);
        nn_epbase_stopped (&self->item.epbase);
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_IDLE, NN_FSM_ACTION, NN_FSM_STOP) {

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        nn_ins_disconnect (&self->item);

        /*  Stop the existing session. */
        nn_assert (!nn_sinproc_isidle (&self->sinproc));
        nn_sinproc_stop (&self->sinproc);

        self->state = NN_CINPROC_STATE_STOPPING;
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_ACTIVE, NN_FSM_ACTION, NN_FSM_STOP) {

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        nn_ins_disconnect (&self->item);

        /*  Stop the existing session. */
        nn_assert (!nn_sinproc_isidle (&self->sinproc));
        nn_sinproc_stop (&self->sinproc);

        self->state = NN_CINPROC_STATE_STOPPING;
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_STOPPING, NN_CINPROC_SRC_SINPROC, NN_SINPROC_STOPPED) {

        self->state = NN_CINPROC_STATE_IDLE;
        nn_fsm_stopped_noevent (&self->fsm);
        nn_epbase_stopped (&self->item.epbase);
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_DISCONNECTED, NN_FSM_ACTION, NN_CINPROC_ACTION_CONNECT) {
        self->state = NN_CINPROC_STATE_ACTIVE;
        nn_epbase_stat_increment (epb, NN_STAT_INPROGRESS_CONNECTIONS, -1);
        nn_epbase_stat_increment (epb, NN_STAT_ESTABLISHED_CONNECTIONS, 1);
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_DISCONNECTED, NN_SINPROC_SRC_PEER, NN_SINPROC_CONNECT) {
        struct nn_sinproc *sinproc = (struct nn_sinproc*) srcptr;
        nn_sinproc_accept (&self->sinproc, sinproc);
        self->state = NN_CINPROC_STATE_ACTIVE;
        nn_epbase_stat_increment (epb, NN_STAT_INPROGRESS_CONNECTIONS, -1);
        nn_epbase_stat_increment (epb, NN_STAT_ESTABLISHED_CONNECTIONS, 1);
        return;
    }

    NN_FSM_JOB (NN_CINPROC_STATE_ACTIVE, NN_CINPROC_SRC_SINPROC, NN_SINPROC_DISCONNECT) {
        self->state = NN_CINPROC_STATE_DISCONNECTED;
        nn_epbase_stat_increment (epb, NN_STAT_INPROGRESS_CONNECTIONS, 1);
        nn_sinproc_init (&self->sinproc, NN_CINPROC_SRC_SINPROC, epb, &self->fsm);
        return;
    }

    nn_fsm_bad_state (self->state, src, type);
}
