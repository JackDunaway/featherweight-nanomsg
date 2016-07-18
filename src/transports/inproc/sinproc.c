/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
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

#include "sinproc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#include <stddef.h>

#define NN_SINPROC_STATE_IDLE 1
#define NN_SINPROC_STATE_CONNECTING 2
#define NN_SINPROC_STATE_READY 3
#define NN_SINPROC_STATE_ACTIVE 4
#define NN_SINPROC_STATE_DISCONNECTED 5
#define NN_SINPROC_STATE_STOPPING_PEER 6
#define NN_SINPROC_STATE_STOPPING 7

#define NN_SINPROC_ACTION_READY 1
#define NN_SINPROC_ACTION_ACCEPTED 2

/*  Set when SENT event was sent to the peer but RECEIVED haven't been
    passed back yet. */
#define NN_SINPROC_FLAG_SENDING 1

/*  Set when SENT event was received, but the new message cannot be written
    to the queue yet, i.e. RECEIVED event haven't been returned
    to the peer yet. */
#define NN_SINPROC_FLAG_RECEIVING 2

/*  Private functions. */
static void nn_sinproc_handler (struct nn_fsm *myfsm, int src, int type,
    void *srcptr);

static int nn_sinproc_send (struct nn_pipebase *mypipe, struct nn_msg *msg);
static int nn_sinproc_recv (struct nn_pipebase *mypipe, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_sinproc_pipebase_vfptr = {
    nn_sinproc_send,
    nn_sinproc_recv
};

void nn_sinproc_init (struct nn_sinproc *self, int src,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    int rcvbuf;
    size_t sz;

    nn_fsm_init (&self->fsm, nn_sinproc_handler, nn_sinproc_handler,
        src, self, owner);
    self->state = NN_SINPROC_STATE_IDLE;
    self->flags = 0;
    self->peer = NULL;
    nn_pipebase_init (&self->pipebase, &nn_sinproc_pipebase_vfptr, epbase);
    sz = sizeof (rcvbuf);
    nn_epbase_getopt (epbase, NN_SOL_SOCKET, NN_RCVBUF, &rcvbuf, &sz);
    nn_assert (sz == sizeof (rcvbuf));
    nn_msgqueue_init (&self->msgqueue, rcvbuf);
    nn_msg_init (&self->msg, 0);
    nn_fsm_event_init (&self->event_connect);
    nn_fsm_event_init (&self->event_sent);
    nn_fsm_event_init (&self->event_received);
    nn_fsm_event_init (&self->event_disconnect);
    nn_list_item_init (&self->item);
}

void nn_sinproc_term (struct nn_sinproc *self)
{
    nn_list_item_term (&self->item);
    nn_fsm_event_term (&self->event_disconnect);
    nn_fsm_event_term (&self->event_received);
    nn_fsm_event_term (&self->event_sent);
    nn_fsm_event_term (&self->event_connect);
    nn_msg_term (&self->msg);
    nn_msgqueue_term (&self->msgqueue);
    nn_pipebase_term (&self->pipebase);
    nn_fsm_term (&self->fsm);
}

int nn_sinproc_isidle (struct nn_sinproc *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_sinproc_connect (struct nn_sinproc *self, struct nn_fsm *peer)
{
    nn_fsm_start (&self->fsm);

    /*  Start the connecting handshake with the peer. */
    nn_fsm_raiseto (&self->fsm, peer, &self->event_connect,
        NN_SINPROC_SRC_PEER, NN_SINPROC_CONNECT, self);
}

void nn_sinproc_accept (struct nn_sinproc *self, struct nn_sinproc *peer)
{
    nn_assert (!self->peer);
    self->peer = peer;

    /*  Start the connecting handshake with the peer. */
    nn_fsm_raiseto (&self->fsm, &peer->fsm, &self->event_connect,
        NN_SINPROC_SRC_PEER, NN_SINPROC_READY, self);

    /*  Notify the state machine. */
    nn_fsm_start (&self->fsm);
    nn_fsm_action (&self->fsm, NN_SINPROC_ACTION_READY);
}

void nn_sinproc_stop (struct nn_sinproc *self)
{
    nn_fsm_stop (&self->fsm);
}

static int nn_sinproc_send (struct nn_pipebase *mypipe, struct nn_msg *msg)
{
    struct nn_sinproc *self = nn_cont (mypipe, struct nn_sinproc, pipebase);
    struct nn_msg nmsg;

    /*  If the peer have already closed the connection, we cannot send
        anymore. */
    if (self->state == NN_SINPROC_STATE_DISCONNECTED)
        return -ECONNRESET;

    /*  Sanity checks. */
    nn_assert_state (self, NN_SINPROC_STATE_ACTIVE);
    nn_assert (!(self->flags & NN_SINPROC_FLAG_SENDING));

    nn_msg_init (&nmsg,
        nn_chunkref_size (&msg->sphdr) +
        nn_chunkref_size (&msg->body));
    memcpy (nn_chunkref_data (&nmsg.body),
        nn_chunkref_data (&msg->sphdr),
        nn_chunkref_size (&msg->sphdr));
    memcpy ((char *)nn_chunkref_data (&nmsg.body) +
        nn_chunkref_size (&msg->sphdr),
        nn_chunkref_data (&msg->body),
        nn_chunkref_size (&msg->body));
    nn_msg_term (msg);

    /*  Expose the message to the peer. */
    nn_msg_term (&self->msg);
    nn_msg_mv (&self->msg, &nmsg);

    /*  Notify the peer that there's a message to get. */
    self->flags |= NN_SINPROC_FLAG_SENDING;
    nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
        &self->peer->event_sent, NN_SINPROC_SRC_PEER,
        NN_SINPROC_SENT, self);

    return 0;
}

static int nn_sinproc_recv (struct nn_pipebase *mypipe, struct nn_msg *msg)
{
    int rc;
    struct nn_sinproc *sinproc;

    sinproc = nn_cont (mypipe, struct nn_sinproc, pipebase);

    /*  Sanity check. */
    nn_assert (sinproc->state == NN_SINPROC_STATE_ACTIVE ||
        sinproc->state == NN_SINPROC_STATE_DISCONNECTED);

    /*  Move the message to the caller. */
    rc = nn_msgqueue_recv (&sinproc->msgqueue, msg);
    errnum_assert (rc == 0, -rc);

    /*  If there was a message from peer lingering because of the exceeded
        buffer limit, try to enqueue it once again. */
    if (sinproc->state != NN_SINPROC_STATE_DISCONNECTED) {
        if (sinproc->flags & NN_SINPROC_FLAG_RECEIVING) {
            rc = nn_msgqueue_send (&sinproc->msgqueue, &sinproc->peer->msg);
            nn_assert (rc == 0 || rc == -EAGAIN);
            if (rc == 0) {
                errnum_assert (rc == 0, -rc);
                nn_msg_init (&sinproc->peer->msg, 0);
                nn_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->peer->event_received, NN_SINPROC_SRC_PEER,
                    NN_SINPROC_RECEIVED, sinproc);
                sinproc->flags &= ~NN_SINPROC_FLAG_RECEIVING;
            }
        }
    }

    if (!nn_msgqueue_empty (&sinproc->msgqueue))
       nn_pipebase_received (&sinproc->pipebase);

    return 0;
}

static void nn_sinproc_handler (struct nn_fsm *myfsm, int src, int type,
    void *srcptr)
{
    struct nn_sinproc *self = nn_cont (myfsm, struct nn_sinproc, fsm);
    int rc;
    int empty;

    NN_FSM_JOB (NN_SINPROC_STATE_IDLE, NN_FSM_ACTION, NN_FSM_STOP) {
        self->state = NN_SINPROC_STATE_STOPPING;
        /*  These events are deemed to be impossible here  */
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));
        nn_fsm_stopped (&self->fsm, NN_SINPROC_STOPPED);
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_CONNECTING, NN_FSM_ACTION, NN_FSM_STOP) {
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
            &self->peer->event_disconnect, NN_SINPROC_SRC_PEER,
            NN_SINPROC_DISCONNECT, self);
        self->state = NN_SINPROC_STATE_STOPPING_PEER;
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_READY, NN_FSM_ACTION, NN_FSM_STOP) {
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
            &self->peer->event_disconnect, NN_SINPROC_SRC_PEER,
            NN_SINPROC_DISCONNECT, self);
        self->state = NN_SINPROC_STATE_STOPPING_PEER;
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_ACTIVE, NN_FSM_ACTION, NN_FSM_STOP) {
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
            &self->peer->event_disconnect, NN_SINPROC_SRC_PEER,
            NN_SINPROC_DISCONNECT, self);
        self->state = NN_SINPROC_STATE_STOPPING_PEER;
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_DISCONNECTED, NN_FSM_ACTION, NN_FSM_STOP) {
        self->state = NN_SINPROC_STATE_STOPPING;
        /*  These events are deemed to be impossible here  */
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));
        nn_fsm_stopped (&self->fsm, NN_SINPROC_STOPPED);
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_STOPPING_PEER, NN_FSM_ACTION, NN_FSM_STOP) {
        self->state = NN_SINPROC_STATE_STOPPING;
        /*  Are all events processed? We can't cancel them unfortunately  */
        if (nn_fsm_event_active (&self->event_received)
            || nn_fsm_event_active (&self->event_disconnect)) {
            return;
        }
        /*  These events are deemed to be impossible here  */
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));

        nn_fsm_stopped (&self->fsm, NN_SINPROC_STOPPED);
        return;
    }
    /* Ignore incoming messages during shutdown. */
    NN_FSM_JOB (NN_SINPROC_STATE_STOPPING_PEER, NN_SINPROC_SRC_PEER, NN_SINPROC_RECEIVED) {
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_STOPPING_PEER, NN_SINPROC_SRC_PEER, NN_SINPROC_DISCONNECT) {
        self->state = NN_SINPROC_STATE_STOPPING;
        /*  Are all events processed? We can't cancel them unfortunately  */
        if (nn_fsm_event_active (&self->event_received)
            || nn_fsm_event_active (&self->event_disconnect)) {
            return;
        }
        /*  These events are deemed to be impossible here  */
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));
        nn_fsm_stopped (&self->fsm, NN_SINPROC_STOPPED);
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_IDLE, NN_FSM_ACTION, NN_FSM_START) {
        self->state = NN_SINPROC_STATE_CONNECTING;
        return;
    }

    /*  CONNECTING - connection request was sent to the peer, and now we are waiting
        for the acknowledgement. */
    NN_FSM_JOB (NN_SINPROC_STATE_CONNECTING, NN_FSM_ACTION, NN_SINPROC_ACTION_READY) {
        self->state = NN_SINPROC_STATE_READY;
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_CONNECTING, NN_SINPROC_SRC_PEER, NN_SINPROC_READY) {
        self->peer = (struct nn_sinproc*) srcptr;
        rc = nn_pipebase_start (&self->pipebase);
        errnum_assert (rc == 0, -rc);
        self->state = NN_SINPROC_STATE_ACTIVE;
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm, &self->event_connect,
            NN_SINPROC_SRC_PEER, NN_SINPROC_ACCEPTED, myfsm);
        return;
    }

    /*  Both peers sent READY so we are both ready for receiving messages. */
    NN_FSM_JOB (NN_SINPROC_STATE_READY, NN_SINPROC_SRC_PEER, NN_SINPROC_READY) {
        rc = nn_pipebase_start (&self->pipebase);
        errnum_assert (rc == 0, -rc);
        self->state = NN_SINPROC_STATE_ACTIVE;
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_READY, NN_SINPROC_SRC_PEER, NN_SINPROC_ACCEPTED) {
        rc = nn_pipebase_start (&self->pipebase);

        /*  TODO: this assertion needs to be replaced with a run-time
            failure. It can be triggered with code disabled within the
            inproc test. */
        errnum_assert (rc == 0, -rc);
        self->state = NN_SINPROC_STATE_ACTIVE;
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_ACTIVE, NN_SINPROC_SRC_PEER, NN_SINPROC_SENT) {
        empty = nn_msgqueue_empty (&self->msgqueue);

        /*  Push the message to the inbound message queue. */
        rc = nn_msgqueue_send (&self->msgqueue,
            &self->peer->msg);
        if (rc == -EAGAIN) {
            self->flags |= NN_SINPROC_FLAG_RECEIVING;
            return;
        }
        errnum_assert (rc == 0, -rc);
        nn_msg_init (&self->peer->msg, 0);

        /*  Notify the user that there's a message to receive. */
        if (empty)
            nn_pipebase_received (&self->pipebase);

        /*  Notify the peer that the message was received. */
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
            &self->peer->event_received, NN_SINPROC_SRC_PEER,
            NN_SINPROC_RECEIVED, self);

        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_ACTIVE, NN_SINPROC_SRC_PEER, NN_SINPROC_RECEIVED) {
        nn_assert (self->flags & NN_SINPROC_FLAG_SENDING);
        nn_pipebase_sent (&self->pipebase);
        self->flags &= ~NN_SINPROC_FLAG_SENDING;
        return;
    }

    NN_FSM_JOB (NN_SINPROC_STATE_ACTIVE, NN_SINPROC_SRC_PEER, NN_SINPROC_DISCONNECT) {
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (&self->fsm, &self->peer->fsm,
            &self->peer->event_disconnect, NN_SINPROC_SRC_PEER,
            NN_SINPROC_DISCONNECT, self);
        self->state = NN_SINPROC_STATE_DISCONNECTED;
        self->peer = NULL;
        nn_fsm_raise (&self->fsm, &self->event_disconnect, NN_SINPROC_DISCONNECT);
        return;
    }

/*  DISCONNECTED: the peer has already closed the connection, but this object
    has not yet been asked to stop. */
    NN_FSM_JOB (NN_SINPROC_STATE_DISCONNECTED, NN_SINPROC_SRC_PEER, NN_SINPROC_RECEIVED) {

        /*  These cases may be safely be ignored. They may happen when
            nn_close() comes before the already enqueued
            NN_SINPROC_RECEIVED has been delivered.  */
        return;
    }
    NN_FSM_JOB (NN_SINPROC_STATE_DISCONNECTED, NN_SINPROC_SRC_PEER, NN_SINPROC_SENT) {

        /*  These cases may be safely be ignored. They may happen when
            nn_close() comes before the already enqueued
            NN_SINPROC_RECEIVED has been delivered.  */
        return;
    }

    nn_fsm_bad_state (self->state, src, type);
}
