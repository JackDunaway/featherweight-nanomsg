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

#include "binproc.h"
#include "sinproc.h"
#include "cinproc.h"
#include "ins.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"

#define NN_FSTATE_BINPROC_IDLE 0x0001
#define NN_FSTATE_BINPROC_ACTIVE 0x0002
#define NN_FSTATE_BINPROC_STOPPING 0x0003

/*  Implementation of nn_epbase interface. */
static void nn_binproc_stop (struct nn_epbase *self);
static void nn_binproc_destroy (struct nn_epbase *self);
static const struct nn_epbase_vfptr nn_binproc_vfptr = {
    nn_binproc_stop,
    nn_binproc_destroy
};

/*  Private functions. */
static void nn_binproc_handler (struct nn_fsm *myfsm, int type, void *srcptr);
static void nn_binproc_connect (struct nn_ins_item *self,
    struct nn_ins_item *peer);

int nn_binproc_create (void *hint, struct nn_epbase **epbase)
{
    int rc;
    struct nn_binproc *self;

    self = nn_alloc (sizeof (struct nn_binproc), "binproc");
    nn_assert_alloc (self);

    nn_ins_item_init (&self->item, &nn_binproc_vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_binproc_handler, nn_binproc_handler,
        nn_epbase_getctx (&self->item.epbase));
    self->state = NN_FSTATE_BINPROC_IDLE;
    nn_list_init (&self->sinprocs);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);

    /*  Register the inproc endpoint into a global repository. */
    rc = nn_ins_bind (&self->item, nn_binproc_connect);
    if (rc < 0) {
        nn_list_term (&self->sinprocs);
        nn_fsm_term_early (&self->fsm);
        nn_ins_item_term (&self->item);
        nn_free (self);
        return rc;
    }

    *epbase = &self->item.epbase;
    return 0;
}

static void nn_binproc_stop (struct nn_epbase *self)
{
    struct nn_binproc *binproc;

    binproc = nn_cont (self, struct nn_binproc, item.epbase);

    nn_fsm_stop (&binproc->fsm);
}

static void nn_binproc_destroy (struct nn_epbase *self)
{
    struct nn_binproc *binproc;

    binproc = nn_cont (self, struct nn_binproc, item.epbase);

    nn_list_term (&binproc->sinprocs);
    nn_fsm_term (&binproc->fsm);
    nn_ins_item_term (&binproc->item);

    nn_free (binproc);
}

static void nn_binproc_destroy_session (struct nn_binproc *self,
    struct nn_sinproc *sinproc)
{
    nn_list_erase (&self->sinprocs, &sinproc->item);
    nn_sinproc_term (sinproc);
    nn_free (sinproc);
}

static void nn_binproc_connect (struct nn_ins_item *self,
    struct nn_ins_item *peer)
{
    struct nn_binproc *binproc;
    struct nn_cinproc *cinproc;
    struct nn_sinproc *sinproc;

    binproc = nn_cont (self, struct nn_binproc, item);
    cinproc = nn_cont (peer, struct nn_cinproc, item);

    nn_assert_state (binproc, NN_FSTATE_BINPROC_ACTIVE);

    sinproc = nn_alloc (sizeof (struct nn_sinproc), "sinproc");
    nn_assert_alloc (sinproc);
    nn_sinproc_init (sinproc, &binproc->item.epbase, &binproc->fsm);
    nn_list_insert_at_end (&binproc->sinprocs, &sinproc->item);
    nn_sinproc_connect (sinproc, &cinproc->fsm);

    nn_epbase_stat_increment (&binproc->item.epbase,
        NN_STAT_ACCEPTED_CONNECTIONS, 1);
}

static void nn_binproc_handler (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_binproc *self = nn_cont (myfsm, struct nn_binproc, fsm);
    struct nn_sinproc *peer;
    struct nn_sinproc *sinproc;
    struct nn_list_item *it;

    switch (self->state | type) {

    /*  Shutdown request has been initiated locally for this bound endpoint. */
    case (NN_FSTATE_BINPROC_ACTIVE | NN_FSM_STOP):

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        nn_ins_unbind (&self->item);

        /*  If there are no active sessions, return early. */
        if (nn_list_empty (&self->sinprocs)) {
            self->state = NN_FSTATE_BINPROC_IDLE;
            nn_fsm_stopped_noevent (&self->fsm);
            nn_epbase_stopped (&self->item.epbase);
            return;
        }

        /*  Command the existing sessions to begin disconnecting. */
        self->state = NN_FSTATE_BINPROC_STOPPING;
        for (it = nn_list_begin (&self->sinprocs);
            it != nn_list_end (&self->sinprocs);
            it = nn_list_next (&self->sinprocs, it)) {
            sinproc = nn_cont (it, struct nn_sinproc, item);
            nn_sinproc_stop (sinproc);
        }

        return;

    /*  One of this binproc's owned connection sessions has been terminated. */
    case (NN_FSTATE_BINPROC_STOPPING | NN_SINPROC_STOPPED):
        sinproc = (struct nn_sinproc*) srcptr;
        //nn_list_erase (&self->sinprocs, &sinproc->item);
        //nn_sinproc_term (sinproc);
        //nn_free (sinproc);
        nn_binproc_destroy_session (self, sinproc);

        /*  Do we need to wait for more sessions to shut down? */
        if (!nn_list_empty (&self->sinprocs)) {
            return;
        }
        self->state = NN_FSTATE_BINPROC_IDLE;
        nn_fsm_stopped_noevent (&self->fsm);
        nn_epbase_stopped (&self->item.epbase);
        return;


    /*  Create a local endpoint that continually listens for remote peer
        connection requests. */
    case (NN_FSTATE_BINPROC_IDLE | NN_FSM_START):
        self->state = NN_FSTATE_BINPROC_ACTIVE;
        return;

    /*  A remote connecting endpoint has initiated a connection request. */
    case (NN_FSTATE_BINPROC_ACTIVE | NN_SINPROC_CONNECTED):
        peer = (struct nn_sinproc*) srcptr;
        sinproc = nn_alloc (sizeof (struct nn_sinproc), "sinproc");
        nn_assert_alloc (sinproc);
        nn_sinproc_init (sinproc, &self->item.epbase, &self->fsm);
        nn_list_insert_at_end (&self->sinprocs, &sinproc->item);
        nn_sinproc_accept (sinproc, &peer->fsm);
        return;

    /*  One of this binproc's owned connection sessions has been
        disconnected. */
    case (NN_FSTATE_BINPROC_ACTIVE | NN_SINPROC_DISCONNECTED):
        nn_epbase_stat_increment (&self->item.epbase,
            NN_STAT_BROKEN_CONNECTIONS, 1);

        //TESTING
        //sinproc = (struct nn_sinproc*) srcptr;
        //nn_binproc_destroy_session (self, sinproc);

        return;

    /*  Immediately after accepting, the remote peer failed the protocol
        negotiation, and our local sinproc session forcibly closed the
        connection. */
    case (NN_FSTATE_BINPROC_ACTIVE | NN_SINPROC_ACCEPT_ERROR):
        nn_epbase_stat_increment (&self->item.epbase,
            NN_STAT_ACCEPT_ERRORS, 1);

        //TESTING
        //sinproc = (struct nn_sinproc*) srcptr;
        //nn_binproc_destroy_session (self, sinproc);

        return;

    default:
        nn_assert_unreachable_fsm (self->state, type);
    }
}

