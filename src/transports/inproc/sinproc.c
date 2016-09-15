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
#define NN_SINPROC_STATE_ACTIVE 4
#define NN_SINPROC_STATE_DISCONNECTED 5
#define NN_SINPROC_STATE_STOPPING_PEER 6
#define NN_SINPROC_STATE_STOPPING 7

#define NN_SINPROC_ACTION_MSG_READY 81
#define NN_SINPROC_ACTION_MSG_RETRIEVED1 91
#define NN_SINPROC_ACTION_MSG_RETRIEVED2 92

/*  Set when SENT event was sent to the peer but RECEIVED haven't been
    passed back yet. */
#define NN_SINPROC_FLAG_SENDING 1

/*  Set when SENT event was received, but the new message cannot be written
    to the queue yet, i.e. RECEIVED event haven't been returned
    to the peer yet. */
#define NN_SINPROC_FLAG_RECEIVING 2

/*  Private functions. */
static void nn_sinproc_handler (struct nn_fsm *myfsm, int type, void *srcptr);

static int nn_sinproc_send (struct nn_pipebase *mypipe, struct nn_msg *msg);
static int nn_sinproc_recv (struct nn_pipebase *mypipe, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_sinproc_pipebase_vfptr = {
    nn_sinproc_send,
    nn_sinproc_recv
};

void nn_sinproc_init (struct nn_sinproc *self,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    int rcvbuf;
    size_t sz;

    nn_fsm_init (&self->fsm, nn_sinproc_handler, nn_sinproc_handler,
        self, owner);
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
    /*  Notify self the connection handshake has started. */
    nn_fsm_start (&self->fsm);

    /*  Start the connecting handshake with the peer. */
    nn_fsm_raiseto (&self->fsm, peer, &self->event_connect,
        NN_SINPROC_CONNECTED, self);
}

void nn_sinproc_accept (struct nn_sinproc *self, struct nn_fsm *peer)
{
    /*  Notify self the connection handshake has started. */
    nn_fsm_start (&self->fsm);

    /*  Start the connecting handshake with the peer. */
    nn_fsm_raiseto (&self->fsm, peer, &self->event_connect,
        NN_SINPROC_ACCEPTED, self);
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
    nn_fsm_raiseto (&self->fsm, &self->peer->fsm, &self->peer->event_sent,
        NN_SINPROC_ACTION_MSG_READY, self);

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

    /*  Move the message to the user. Failure, including EAGAIN, is never
        expected here. */
    rc = nn_msgqueue_dequeue (&sinproc->msgqueue, msg);
    errnum_assert (rc == 0, -rc);

    /*  If there was a message from peer lingering because of the exceeded
        buffer limit, try to enqueue it once again. */
    if (sinproc->state != NN_SINPROC_STATE_DISCONNECTED) {
        if (sinproc->flags & NN_SINPROC_FLAG_RECEIVING) {
            rc = nn_msgqueue_enqueue (&sinproc->msgqueue, &sinproc->peer->msg);
            if (rc == 0) {
                nn_msg_init (&sinproc->peer->msg, 0);
                nn_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->peer->event_received,
                    NN_SINPROC_ACTION_MSG_RETRIEVED1, sinproc);
                sinproc->flags &= ~NN_SINPROC_FLAG_RECEIVING;
            }
            else {
                nn_assert (rc == -EAGAIN);
            }
        }
    }

    if (!nn_msgqueue_empty (&sinproc->msgqueue)) {
        nn_pipebase_received (&sinproc->pipebase);
    }

    return 0;
}

static void nn_sinproc_handler (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_sinproc *self = nn_cont (myfsm, struct nn_sinproc, fsm);
    struct nn_fsm *peerfsm = self->peer ? &self->peer->fsm : NULL;
    int rc;
    int empty;

    switch (self->state | type) {

    /*  asdf */
    case (NN_SINPROC_STATE_CONNECTING | NN_FSM_STOP):
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (myfsm, peerfsm, &self->peer->event_disconnect,
            NN_SINPROC_DISCONNECTED, self);
        self->state = NN_SINPROC_STATE_STOPPING_PEER;
        return;

    /*  Shutdown request has been initiated for this active session. */
    case (NN_SINPROC_STATE_ACTIVE | NN_FSM_STOP):
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (myfsm, peerfsm, &self->peer->event_disconnect,
            NN_SINPROC_DISCONNECTED, self);
        self->state = NN_SINPROC_STATE_STOPPING_PEER;
        return;

    /*  asdf */
    case (NN_SINPROC_STATE_DISCONNECTED | NN_FSM_STOP):
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));
        nn_assert (!nn_fsm_event_active (&self->event_disconnect));
        nn_assert (!nn_fsm_event_active (&self->event_received));
        self->state = NN_SINPROC_STATE_STOPPING;
        nn_fsm_stopped (myfsm, NN_SINPROC_STOPPED);
        return;

    /*  asdf */
    case (NN_SINPROC_STATE_STOPPING_PEER | NN_FSM_STOP):
        /*  Are all events processed? We can't cancel them unfortunately  */
        if (nn_fsm_event_active (&self->event_received)
            || nn_fsm_event_active (&self->event_disconnect)) {

            nn_assert_unreachable ("JRD - do we hit this?");

            return;
        }
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));

        self->state = NN_SINPROC_STATE_STOPPING;
        nn_fsm_stopped (myfsm, NN_SINPROC_STOPPED);
        return;

    /*  asdf */
    case (NN_SINPROC_STATE_STOPPING_PEER | NN_SINPROC_DISCONNECTED):
        /*  Are all events processed? We can't cancel them unfortunately  */
        if (nn_fsm_event_active (&self->event_received)
            || nn_fsm_event_active (&self->event_disconnect)) {

            nn_assert_unreachable ("JRD - do we hit this?");

            return;
        }
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_sent));
        self->state = NN_SINPROC_STATE_STOPPING;
        nn_fsm_stopped (myfsm, NN_SINPROC_STOPPED);
        return;

    /*  Activate a newly-created session. */
    case (NN_SINPROC_STATE_IDLE | NN_FSM_START):
        self->state = NN_SINPROC_STATE_CONNECTING;
        return;

    /*  Create session on bound side after accepting remote connection request. */
    case (NN_SINPROC_STATE_CONNECTING | NN_SINPROC_ACCEPTED):
        nn_assert (self->peer == NULL);
        self->peer = (struct nn_sinproc*) srcptr;
        rc = nn_pipebase_start (&self->pipebase);
        errnum_assert (rc == 0, -rc);
        self->state = NN_SINPROC_STATE_ACTIVE;
        nn_fsm_raiseto (myfsm, &self->peer->fsm, &self->event_connect,
            NN_SINPROC_ESTABLISHED, myfsm);
        return;

    /*  This session, created by a connecting endpoint, has received notification
        from bound peer that the connection is ready. */
    case (NN_SINPROC_STATE_CONNECTING | NN_SINPROC_ESTABLISHED):
        nn_assert (self->peer == NULL);
        self->peer = (struct nn_sinproc*) srcptr;
        rc = nn_pipebase_start (&self->pipebase);
        if (rc == 0) {
            self->state = NN_SINPROC_STATE_ACTIVE;
            return;
        }
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (myfsm, &self->peer->fsm,
            &self->peer->event_disconnect,
            NN_SINPROC_ACCEPT_ERROR, self);
        self->state = NN_SINPROC_STATE_DISCONNECTED;
        nn_fsm_raise (myfsm, &self->event_sent, NN_SINPROC_ACCEPT_ERROR);
        return;

    /*  This local session endpoint has received a message sent by the remote
        session endpoint. */
    case (NN_SINPROC_STATE_ACTIVE | NN_SINPROC_ACTION_MSG_READY):
        /*  Remember if the queue is initially empty. If so, signal user a
            message is ready to receive. */
        empty = nn_msgqueue_empty (&self->msgqueue);

        /*  Retrieve message from peer and enqueue into our inbound message queue. */
        rc = nn_msgqueue_enqueue (&self->msgqueue, &self->peer->msg);
        if (rc == -EAGAIN) {
            self->flags |= NN_SINPROC_FLAG_RECEIVING;
            return;
        }
        errnum_assert (rc == 0, -rc);
        nn_msg_init (&self->peer->msg, 0);

        /*  Notify the user that there's a message to receive, yet do not
            reassert if already signalled. */
        if (empty) {
            nn_pipebase_received (&self->pipebase);
        }

        /*  Notify the peer that the message was retrieved. */
        nn_fsm_raiseto (myfsm, &self->peer->fsm, &self->peer->event_received,
            NN_SINPROC_ACTION_MSG_RETRIEVED2, self);

        return;

    /*  The remote session endpoint has acknowledged receipt of the message
        this local endpoint just sent. */
    case (NN_SINPROC_STATE_ACTIVE | NN_SINPROC_ACTION_MSG_RETRIEVED1):
        nn_assert (self->flags & NN_SINPROC_FLAG_SENDING);
        nn_pipebase_sent (&self->pipebase);
        self->flags &= ~NN_SINPROC_FLAG_SENDING;
        return;
    case (NN_SINPROC_STATE_ACTIVE | NN_SINPROC_ACTION_MSG_RETRIEVED2):
        nn_assert (self->flags & NN_SINPROC_FLAG_SENDING);
        nn_pipebase_sent (&self->pipebase);
        self->flags &= ~NN_SINPROC_FLAG_SENDING;
        return;

    /*  The session created by a bound endpoint received notification from its
        connected peer that it is disconnecting. */
    case (NN_SINPROC_STATE_ACTIVE | NN_SINPROC_DISCONNECTED):
        nn_pipebase_stop (&self->pipebase);
        nn_fsm_raiseto (myfsm, &self->peer->fsm, &self->peer->event_disconnect,
            NN_SINPROC_DISCONNECTED, self);
        self->state = NN_SINPROC_STATE_DISCONNECTED;
        nn_assert (!nn_fsm_event_active (&self->event_disconnect));
        nn_fsm_raise (myfsm, &self->event_disconnect, NN_SINPROC_DISCONNECTED);
        return;

    /*  The session created by a bound endpoint is being disconnected because
        the peer failed during initial protocol negotiation. */
    case (NN_SINPROC_STATE_DISCONNECTED | NN_SINPROC_DISCONNECTED):
        nn_assert (self->peer);
        self->peer = NULL;
        nn_assert (!nn_fsm_event_active (&self->event_disconnect));
        nn_fsm_raise (myfsm, &self->event_disconnect, NN_SINPROC_DISCONNECTED);
        return;

    /*  An accept error from a peer is a connect error for this side. */
    case (NN_SINPROC_STATE_ACTIVE | NN_SINPROC_ACCEPT_ERROR):
        nn_assert (!nn_fsm_event_active (&self->event_connect));
        nn_assert (!nn_fsm_event_active (&self->event_disconnect));
        nn_assert (!nn_fsm_event_active (&self->event_received));
        nn_assert (!nn_fsm_event_active (&self->event_sent));

        nn_sinproc_stop (self);
        self->state = NN_SINPROC_STATE_STOPPING;
        nn_fsm_stopped (myfsm, NN_SINPROC_CONNECT_ERROR);
        return;

    default:
        nn_assert_unreachable_fsm (self->state, type);
    }
}
