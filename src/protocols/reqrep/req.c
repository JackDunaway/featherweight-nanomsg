/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
    Copyright 2016 Garrett D'Amore

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

#include "req.h"
#include "xreq.h"

#include "../../nn.h"
#include "../../reqrep.h"

#include "../../aio/fsm.h"
#include "../../aio/worker.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/random.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

#include <stddef.h>
#include <string.h>

/*  Default re-send interval is 1 minute. */
#define NN_REQ_DEFAULT_RESEND_IVL 60000

/*  Finite states unique to this state machine. */
#define NN_STATE_REQ_IDLE 0x0001
#define NN_STATE_REQ_READY_TO_REQUEST 0x0002
#define NN_STATE_REQ_WAITING_FOR_PIPE 0x0003
#define NN_STATE_REQ_REQUEST_IN_FLIGHT 0x0004
#define NN_STATE_REQ_PREPARING_TO_RESEND 0x0005
#define NN_STATE_REQ_CANCELLING 0x0006
#define NN_STATE_REQ_FINALIZING_ROUND_TRIP 0x0007
#define NN_STATE_REQ_REPLY_ARRIVED 0x0008
#define NN_STATE_REQ_STOPPING 0x0009

/*  State machine notifications unique to the REQ object. */
#define NN_NOTIFY_USER_SUBMITTED_REQ 0x00510000
#define NN_NOTIFY_USER_RETRIEVED_REP 0x00520000
#define NN_NOTIFY_RESEND 0x00530000
#define NN_NOTIFY_OUTGOING_PIPE_AVAILABLE 0x00540000
#define NN_NOTIFY_REPLY_ARRIVED 0x00550000

static const struct nn_sockbase_vfptr nn_req_sockbase_vfptr = {
    nn_req_stop,
    nn_req_destroy,
    nn_xreq_add,
    nn_req_rm,
    nn_req_in,
    nn_req_out,
    nn_req_events,
    nn_req_csend,
    nn_req_crecv,
    nn_req_setopt,
    nn_req_getopt
};

void nn_req_init (struct nn_req *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint)
{
    nn_xreq_init (&self->xreq, vfptr, hint);
    nn_fsm_init_root (&self->fsm, nn_req_handler, nn_req_handler,
        nn_sockbase_getctx (&self->xreq.sockbase));
    self->state = NN_STATE_REQ_IDLE;

    /*  Start assigning request IDs beginning with a random number. This way
        there should be no key clashes even if the executable is re-started. */
    nn_random_generate (&self->currentid, sizeof (self->currentid));

    self->resend_ivl = NN_REQ_DEFAULT_RESEND_IVL;
    self->via = NULL;
    nn_timer_init (&self->timer, &self->fsm);
    nn_msg_init (&self->request, 0);
    nn_msg_init (&self->reply, 0);

    /*  Start the state machine. */
    nn_fsm_start (&self->fsm);
}

void nn_req_term (struct nn_req *self)
{
    nn_timer_term (&self->timer);
    nn_msg_term (&self->reply);
    nn_msg_term (&self->request);
    nn_fsm_term (&self->fsm);
    nn_xreq_term (&self->xreq);
}

void nn_req_stop (struct nn_sockbase *self)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    nn_fsm_stop (&req->fsm);
}

void nn_req_destroy (struct nn_sockbase *self)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    nn_req_term (req);
    nn_free (req);
}

int nn_req_inprogress (struct nn_req *self)
{
    /*  Return 1 if there's a request submitted. 0 otherwise. */
    return self->state == NN_STATE_REQ_IDLE ||
        self->state == NN_STATE_REQ_READY_TO_REQUEST ||
        self->state == NN_STATE_REQ_STOPPING ? 0 : 1;
}

void nn_req_in (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);
    uint32_t reqid;
    int rc;

    /*  Pass the pipe to the raw REQ socket. */
    nn_xreq_in (&req->xreq.sockbase, pipe);

    while (1) {

        /*  Get new reply. */
        rc = nn_xreq_recv (&req->xreq.sockbase, &req->reply);
        if (rc == -EAGAIN)
            return;
        errnum_assert (rc == 0, -rc);

        /*  No request was sent. Getting a reply doesn't make sense. */
        if (!nn_req_inprogress (req)) {
            nn_msg_term (&req->reply);
            nn_msg_init (&req->reply, 0);
            continue;
        }

        /*  Ignore malformed replies. */
        if (nn_chunkref_size (&req->reply.sphdr) != NN_WIRE_REQID_LEN) {
            nn_msg_term (&req->reply);
            nn_msg_init (&req->reply, 0);
            continue;
        }

        /*  Ignore replies with incorrect request IDs. */
        reqid = nn_getl (nn_chunkref_data (&req->reply.sphdr));
        if (!nn_reqid_is_final (reqid)) {
            nn_msg_term (&req->reply);
            nn_msg_init (&req->reply, 0);
            continue;
        }
        if (req->currentid != reqid) {
            nn_msg_term (&req->reply);
            nn_msg_init (&req->reply, 0);
            continue;
        }

        /*  Trim the request ID. */
        nn_chunkref_term (&req->reply.sphdr);
        nn_chunkref_init (&req->reply.sphdr, 0);

        /*  TODO: Deallocate the request here? */

        /*  Notify the state machine. */
        //if (req->state == NN_REQ_STATE_ACTIVE) {
            nn_fsm_do_now (&req->fsm, NN_NOTIFY_REPLY_ARRIVED);
        //}

        return;
    }
}

void nn_req_out (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    /*  Add the pipe to the underlying raw socket. */
    nn_xreq_out (&req->xreq.sockbase, pipe);

    /*  Notify the state machine, but only if its actions have been deferred
        waiting for a new outbound pipe. */
    if (req->state == NN_STATE_REQ_WAITING_FOR_PIPE) {
        nn_fsm_do_now (&req->fsm, NN_NOTIFY_OUTGOING_PIPE_AVAILABLE);
    }
}

int nn_req_events (struct nn_sockbase *self)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);
    int rc;

    /*  OUT is signalled all the time because sending a request while
        another one is being processed cancels the old one. */
    rc = NN_SOCKBASE_EVENT_OUT;

    /*  In DONE state the reply is stored in 'reply' field. */
    if (req->state == NN_STATE_REQ_REPLY_ARRIVED) {
        rc |= NN_SOCKBASE_EVENT_IN;
    }

    return rc;
}

int nn_req_csend (struct nn_sockbase *self, struct nn_msg *msg)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    /*  Generate new Request ID.  */
    req->currentid = nn_reqid_next (req->currentid);

    /*  Tag the request with the Request ID. */
    nn_assert (nn_chunkref_size (&msg->sphdr) == 0);
    nn_chunkref_term (&msg->sphdr);
    nn_chunkref_init (&msg->sphdr, NN_WIRE_REQID_LEN);
    nn_putl (nn_chunkref_data (&msg->sphdr), req->currentid);

    /*  Store the request so that it can be re-sent if there's no reply. */
    nn_msg_term (&req->request);
    nn_msg_mv (&req->request, msg);

    /*  Notify the state machine. */
    nn_fsm_do_now (&req->fsm, NN_NOTIFY_USER_SUBMITTED_REQ);

    return 0;
}

int nn_req_crecv (struct nn_sockbase *self, struct nn_msg *msg)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    /*  No request was sent. Waiting for a reply doesn't make sense. */
    if (!nn_req_inprogress (req))
        return -EFSM;

    /*  If reply was not yet received, wait further. */
    if (req->state != NN_STATE_REQ_REPLY_ARRIVED)
        return -EAGAIN;

    /*  Pass cached reply to the caller and reinitialize for next round. */
    nn_msg_mv (msg, &req->reply);
    nn_msg_init (&req->reply, 0);

    /*  Notify the state machine. */
    nn_fsm_do_now (&req->fsm, NN_NOTIFY_USER_RETRIEVED_REP);

    return 0;
}

int nn_req_setopt (struct nn_sockbase *self, int level, int option,
        const void *optval, size_t optvallen)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    if (level != NN_REQ)
        return -ENOPROTOOPT;

    switch (option) {
    case NN_REQ_RESEND_IVL:
        if (optvallen != sizeof (int))
            return -EINVAL;
        req->resend_ivl = *(int*) optval;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

int nn_req_getopt (struct nn_sockbase *self, int level, int option,
        void *optval, size_t *optvallen)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    if (level != NN_REQ)
        return -ENOPROTOOPT;

    switch (option) {
    case NN_REQ_RESEND_IVL:
        if (*optvallen < sizeof (int))
            return -EINVAL;
        *(int*) optval = req->resend_ivl;
        *optvallen = sizeof (int);
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

void nn_req_action_send (struct nn_req *self)
{
    int rc;
    struct nn_msg msg;
    struct nn_pipe *via;

    nn_assert (self->via == NULL);

    /*  Send the request. */
    nn_msg_cp (&msg, &self->request);
    rc = nn_xreq_send_to (&self->xreq.sockbase, &msg, &via);
    if (rc == 0) {
        /*  Request was successfully sent. Set up the resend timer in case
            the request gets lost somewhere further out in the topology. */
        nn_timer_start (&self->timer, NN_NOTIFY_RESEND, self->resend_ivl);
        nn_assert (via);
        self->via = via;
        self->state = NN_STATE_REQ_REQUEST_IN_FLIGHT;
        return;
    }

    /*  If not immediate success, we expect to be able to try again, and no
        other type of failure. */
    errnum_assert (rc == -EAGAIN, -rc);

    /*  If already waiting for a pipe, it's expected that this function would
        not have been called unless the send would immediately succeeded. */
    nn_assert (self->state != NN_STATE_REQ_WAITING_FOR_PIPE);

    /*  Enter waiting state for a new outbound pipe to arrive. */
    nn_msg_term (&msg);
    self->state = NN_STATE_REQ_WAITING_FOR_PIPE;

    return;
}

static int nn_req_create (void *hint, struct nn_sockbase **sockbase)
{
    struct nn_req *self;

    self = nn_alloc (sizeof (struct nn_req), "socket (req)");
    nn_assert_alloc (self);
    nn_req_init (self, &nn_req_sockbase_vfptr, hint);
    *sockbase = &self->xreq.sockbase;

    return 0;
}

void nn_req_rm (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_req *req = nn_cont (self, struct nn_req, xreq.sockbase);

    nn_xreq_rm (self, pipe);
    if (pipe == req->via) {
        nn_fsm_do_now (&req->fsm, NN_EVENT_PIPE_GONE);
    }
}

void nn_req_shutdown (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_req *req;

    req = nn_cont (self, struct nn_req, fsm);
    nn_assert (srcptr == NULL);

    if (type == NN_FSM_STOP) {
        nn_timer_cancel (&req->timer);
        req->state = NN_STATE_REQ_STOPPING;
    }
    if (req->state == NN_STATE_REQ_STOPPING) {
        if (!nn_timer_isidle (&req->timer))
            return;
        req->state = NN_STATE_REQ_IDLE;
        nn_fsm_stopped_noevent (&req->fsm);
        nn_sockbase_stopped (&req->xreq.sockbase);
        return;
    }

    nn_assert_unreachable_fsm (req->state, type);
}

void nn_req_handler (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_req *self = nn_cont (myfsm, struct nn_req, fsm);

    switch (self->state | type) {

    case (NN_STATE_REQ_IDLE | NN_FSM_START):
        /*  Enter state to wait for a request to be submitted by user. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_READY_TO_REQUEST;
        return;

    case (NN_STATE_REQ_READY_TO_REQUEST | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  A request has been submitted by the user. */
        nn_assert (srcptr == NULL);
        nn_req_action_send (self);
        return;

    case (NN_STATE_REQ_WAITING_FOR_PIPE | NN_NOTIFY_OUTGOING_PIPE_AVAILABLE):
        /*  A pending request may now be sent now that a peer has arrived. */
        nn_assert (srcptr == NULL);
        nn_req_action_send (self);
        return;

    case (NN_STATE_REQ_PREPARING_TO_RESEND | NN_EVENT_TIMER_STOPPED):
        /*  Now that the timer is idle, we can resend the request. */
        nn_assert (srcptr == &self->timer);
        nn_req_action_send (self);
        return;

    case (NN_STATE_REQ_CANCELLING | NN_EVENT_TIMER_STOPPED):
        /*  Now that the previous resend timer is stopped, we can send the new
            request that has superceded the previous request. */
        nn_assert (srcptr == &self->timer);
        nn_req_action_send (self);
        return;

    case (NN_STATE_REQ_REPLY_ARRIVED | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  The waiting reply from the previous successful request has been
            effectively abandoned since the user just sent a new request. */
        nn_assert (srcptr == NULL);
        nn_req_action_send (self);
        return;

    case (NN_STATE_REQ_WAITING_FOR_PIPE | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  New request was submitted by user while still waiting for a peer.
            Take no action immediately and remain in waiting state. */
        nn_assert (srcptr == NULL);
        return;

    case (NN_STATE_REQ_REQUEST_IN_FLIGHT | NN_NOTIFY_REPLY_ARRIVED):
        /*  Reply arrived. */
        nn_assert (srcptr == NULL);
        nn_timer_cancel (&self->timer);
        self->via = NULL;
        self->state = NN_STATE_REQ_FINALIZING_ROUND_TRIP;
        return;

    case (NN_STATE_REQ_REQUEST_IN_FLIGHT | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  New request was sumbitted while the previous one was still active,
            so we must abandon and cancel the previous request. */
        nn_assert (srcptr == NULL);
        nn_timer_cancel (&self->timer);
        self->via = NULL;
        self->state = NN_STATE_REQ_CANCELLING;
        return;

    case (NN_STATE_REQ_REQUEST_IN_FLIGHT | NN_EVENT_PIPE_GONE):
        /*  Pipe originally used to send request via is no longer available. */
        nn_assert (srcptr == NULL);
        nn_timer_cancel (&self->timer);
        self->via = NULL;
        self->state = NN_STATE_REQ_PREPARING_TO_RESEND;
        return;

    case (NN_STATE_REQ_REQUEST_IN_FLIGHT | NN_NOTIFY_RESEND):
        /*  Resend interval has expired, so prepare to reset timer and resend. */
        nn_assert (srcptr == &self->timer);
        nn_timer_cancel (&self->timer);
        self->via = NULL;
        self->state = NN_STATE_REQ_PREPARING_TO_RESEND;
        return;

    case (NN_STATE_REQ_PREPARING_TO_RESEND | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  New request was submitted while the previous request was about to
            attempt a resend (for one a few different reasons), so we must
            abandon and cancel the retry attempt of the previous request. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_CANCELLING;
        return;

    case (NN_STATE_REQ_CANCELLING | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  No need to do anything here. The abandoned delayed request has been
            replaced by the new one that will be sent once the previous timer
            has stopped. */
        nn_assert (srcptr == NULL);
        return;

    case (NN_STATE_REQ_FINALIZING_ROUND_TRIP | NN_EVENT_TIMER_STOPPED):
        /*  Reply has arrived and the resend timer is inactive; enter a passive
            state waiting for the user to retrieve the message. */
        nn_assert (srcptr == &self->timer);
        self->state = NN_STATE_REQ_REPLY_ARRIVED;
        return;

    case (NN_STATE_REQ_FINALIZING_ROUND_TRIP | NN_NOTIFY_USER_SUBMITTED_REQ):
        /*  Prior to retrieving the successful reply, the user has submitted a
            new request, so we must abandon the successful request/reply round
            trip. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_CANCELLING;
        return;

    case (NN_STATE_REQ_REPLY_ARRIVED | NN_NOTIFY_USER_RETRIEVED_REP):
        /*  The waiting reply has been retrieved by the user. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_READY_TO_REQUEST;
        return;

    case (NN_STATE_REQ_READY_TO_REQUEST | NN_FSM_STOP):
    case (NN_STATE_REQ_WAITING_FOR_PIPE | NN_FSM_STOP):
    case (NN_STATE_REQ_REPLY_ARRIVED | NN_FSM_STOP):
        /*  Shutdown requested while no request was active, so a synchronous
            shutdown is possible. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_IDLE;
        nn_fsm_stopped_noevent (&self->fsm);
        nn_sockbase_stopped (&self->xreq.sockbase);
        return;

    case (NN_STATE_REQ_PREPARING_TO_RESEND | NN_FSM_STOP):
    case (NN_STATE_REQ_CANCELLING | NN_FSM_STOP):
    case (NN_STATE_REQ_FINALIZING_ROUND_TRIP | NN_FSM_STOP):
        /*  Shutdown requested while a request was active but not in flight,
            so enter state waiting for the timer to stop. */
        nn_assert (srcptr == NULL);
        self->state = NN_STATE_REQ_STOPPING;
        return;

    case (NN_STATE_REQ_REQUEST_IN_FLIGHT | NN_FSM_STOP):
        /*  Shutdown requested while a request in flight, so stop the timer
            and begin waiting for it to stop. */
        nn_assert (srcptr == NULL);
        nn_timer_cancel (&self->timer);
        self->state = NN_STATE_REQ_STOPPING;
        return;

    case (NN_STATE_REQ_STOPPING | NN_EVENT_TIMER_STOPPED):
        /*  Asynchronous shutdown is now complete. */
        nn_assert (srcptr == &self->timer);
        self->state = NN_STATE_REQ_IDLE;
        nn_fsm_stopped_noevent (&self->fsm);
        nn_sockbase_stopped (&self->xreq.sockbase);
        return;

    default:
        nn_assert_unreachable_fsm (self->state, type);
        return;
    }
}

static struct nn_socktype nn_req_socktype_struct = {
    AF_SP,
    NN_REQ,
    0,
    nn_req_create,
    nn_xreq_ispeer,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_socktype *nn_req_socktype = &nn_req_socktype_struct;
