/*
    Copyright (c) 2016 Jack R. Dunaway. All rights reserved.

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

#include "ustream.h"

#include "../../utils/cont.h"
#include "../../utils/err.h"

/*  Private functions. */
static void nn_stream_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_stream_shutdown (struct nn_fsm *self, int type, void *srcptr);


void nn_stream_init (struct nn_stream *self, struct nn_fsm *owner,
    struct nn_stream_vfptr *vft)
{
    /*  Require stream subclasses to implement the core subset of overrides. */
    nn_assert (vft->close);

    nn_fsm_init (&self->fsm, nn_stream_handler, nn_stream_shutdown,
        self, owner);

    /*  Choose a worker thread to handle this stream. */
    self->worker = nn_worker_choose (&self->fsm);

    self->vft;
    self->state = NN_USOCK_STATE_IDLE;
    self->fd = NN_INVALID_FD;
    //nn_task_io_init (&self->incoming, &self->fsm);
    //nn_task_io_init (&self->outgoing, &self->fsm);
    nn_task_io_init (&self->task_connecting, &self->fsm);
    nn_task_io_init (&self->task_connected, &self->fsm);
    nn_task_io_init (&self->task_accept, &self->fsm);
    nn_task_io_init (&self->task_send, &self->fsm);
    nn_task_io_init (&self->task_recv, &self->fsm);
    nn_task_io_init (&self->task_stop, &self->fsm);

    /*  Intialise events raised by the underlying stream to its owning FSM. */
    nn_fsm_event_init (&self->established);
    nn_fsm_event_init (&self->sent);
    nn_fsm_event_init (&self->received);
    nn_fsm_event_init (&self->errored);

    /*  No accepting is going on at the moment. */
    self->asock = NULL;
}

void nn_stream_term (struct nn_stream *self)
{
    nn_assert_state (self, NN_USOCK_STATE_IDLE);

    nn_fsm_event_term (&self->errored);
    nn_fsm_event_term (&self->received);
    nn_fsm_event_term (&self->sent);
    nn_fsm_event_term (&self->established);
    //nn_task_io_term (&self->outgoing);
    //nn_task_io_term (&self->incoming);

    nn_task_io_cancel (&self->task_recv);


    nn_task_io_term (&self->task_stop);
    nn_task_io_term (&self->task_recv);
    nn_task_io_term (&self->task_send);
    nn_task_io_term (&self->task_accept);
    nn_task_io_term (&self->task_connected);
    nn_task_io_term (&self->task_connecting);

    nn_fsm_term (&self->fsm);
}

void nn_stream_swap_owner (struct nn_stream *self, struct nn_fsm *newowner)
{
    nn_fsm_swap_owner (&self->fsm, newowner);
}

int nn_stream_pending (struct nn_stream *self)
{
    return (nn_worker_io_isidle (&self->incoming) &&
        nn_worker_io_isidle (&self->outgoing)) ? 1 : 0;
}

void nn_stream_activate (struct nn_stream *self)
{
}

int nn_stream_isidle (struct nn_stream *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_stream_stop (struct nn_stream *self)
{
    nn_fsm_stop (&self->fsm);
}

static void nn_stream_shutdown (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_stream *self = nn_cont (myfsm, struct nn_stream, fsm);

    if (type == NN_FSM_STOP) {

        /*  Stream cannot be shutdown while an actively accepting. */
        nn_assert (self->state != NN_USOCK_STATE_ACCEPTING &&
            self->state != NN_USOCK_STATE_CANCELLING_ACCEPT);

        /*  Synchronous stop. */
        if (self->state == NN_USOCK_STATE_IDLE)
            goto finish3;
        if (self->state == NN_USOCK_STATE_DONE)
            goto finish2;
        if (self->state == NN_USOCK_STATE_STARTING ||
            self->state == NN_USOCK_STATE_ACCEPTED ||
            self->state == NN_USOCK_STATE_ACCEPTING_ERROR ||
            self->state == NN_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (self->state == NN_USOCK_STATE_BEING_ACCEPTED) {
            nn_fsm_do_now (self->asock, NN_STREAM_CANCEL_ACCEPT);
            self->state = NN_USOCK_STATE_CANCELLING_ACCEPT;
            return;
        }

        /*  If we were already in the process of cancelling overlapped
            operations, we don't have to do anything. Continue waiting
            till cancelling is finished. */
        if (self->state == NN_USOCK_STATE_CANCELLING_IO) {
            self->state = NN_USOCK_STATE_STOPPING;
            return;
        }

        /*  Notify our parent that pipe socket is shutting down  */
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_SHUTDOWN);
        self->state = NN_USOCK_STATE_STOPPING;

        /*  In all remaining states we'll simply cancel all overlapped
            operations. */
        if (self->vft->cancel_io (self) == 0)
            goto finish1;
        return;
    }
    if (self->state == NN_USOCK_STATE_CANCELLING_ACCEPT) {
        nn_assert (type == NN_STREAM_ACCEPT_ERROR);
        goto finish1;
    }
    if (self->state == NN_USOCK_STATE_STOPPING) {
        if (nn_stream_pending (self)) {
            return;
        }
    finish1:
        self->vft->close (self);
    finish2:
        self->state = NN_USOCK_STATE_IDLE;
        nn_fsm_stopped (&self->fsm, NN_STREAM_STOPPED);
    finish3:
        return;
    }

    nn_assert_unreachable_fsm (self->state, type);
}

static void nn_stream_handler (struct nn_fsm *myfsm, int type, void *srcptr)
{
    struct nn_stream *self = nn_cont (myfsm, struct nn_stream, fsm);

    switch (self->state | type) {

    case (NN_USOCK_STATE_IDLE | NN_FSM_START):
        /*  Underlying OS file descriptor is created, but it's not yet been
            associated with and endpoint, and as such has no I/O operations
            registered with the worker thread. In this state we can set socket
            options, local and remote address, etc. */
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_STARTING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_LISTENING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_LISTENING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_CONNECTING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_CONNECTING;
        return;

    case (NN_USOCK_STATE_STARTING | NN_STREAM_START_BEING_ACCEPTED):
        /*  accept() was called on the usock. Now the socket is waiting for a
            new connection to arrive. */
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_BEING_ACCEPTED;
        return;

    case (NN_USOCK_STATE_BEING_ACCEPTED | NN_STREAM_ACCEPTED):
        /*  Connection was accepted and can now be tuned. */
        nn_assert (srcptr == NULL);
        nn_assert (self->asock);
        self->asock = NULL;
        self->state = NN_USOCK_STATE_ACCEPTED;
        nn_fsm_raise (&self->fsm, &self->established, NN_STREAM_ACCEPTED);
        return;

    case (NN_USOCK_STATE_ACCEPTED | NN_STREAM_ACTIVATE):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACTIVE;
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_CONNECTED):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACTIVE;
        nn_fsm_raise (&self->fsm, &self->established, NN_STREAM_CONNECTED);
        return;

    case (NN_USOCK_STATE_CONNECTING | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        self->vft->close (self);
        self->state = NN_USOCK_STATE_DONE;
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_ERROR);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_RECEIVED):
        nn_assert (srcptr == NULL);
        nn_fsm_raise (&self->fsm, &self->received, NN_STREAM_RECEIVED);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_SENT):
        nn_assert (srcptr == NULL);
        if (self->vft->sent) {
            self->vft->sent (self);
        }
        nn_fsm_raise (&self->fsm, &self->sent, NN_STREAM_SENT);
        return;

    case (NN_USOCK_STATE_ACTIVE | NN_STREAM_ERROR):
        nn_assert (srcptr == NULL);
        if (self->vft->cancel_io (self) == 0) {
            nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_SHUTDOWN);
            self->vft->close (self);
            self->state = NN_USOCK_STATE_DONE;
            return;
        }
        self->state = NN_USOCK_STATE_CANCELLING_IO;
        return;

    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_RECEIVED):
    case (NN_USOCK_STATE_CANCELLING_IO | NN_STREAM_SENT):
        nn_assert (srcptr == NULL);
        /*  Lingering I/O events from worker are ignored during shutdown. */
        if (nn_stream_pending (self)) {
            return;
        }
        nn_fsm_raise (&self->fsm, &self->errored, NN_STREAM_SHUTDOWN);
        self->vft->close (self);
        self->state = NN_USOCK_STATE_DONE;
        return;

    case (NN_USOCK_STATE_LISTENING | NN_STREAM_START_ACCEPTING):
        nn_assert (srcptr == NULL);
        self->state = NN_USOCK_STATE_ACCEPTING;
        return;

    case (NN_USOCK_STATE_ACCEPTING | NN_STREAM_CANCEL_ACCEPT):
        nn_assert (srcptr == NULL);
        self->vft->cancel_io (self);
        self->state = NN_USOCK_STATE_CANCELLING_ACCEPT;
        return;

    case (NN_USOCK_STATE_ACCEPTING | NN_STREAM_ACCEPTED):
        nn_assert (srcptr == self->asock);
        nn_task_io_finish (&self->established, srcptr);
        /*  Notify new stream object it has been accepted. */
        nn_fsm_do_now (self->asock, NN_STREAM_ACCEPTED);

        /*  Disassociate the listener socket from the accepted socket. */
        self->asock = NULL;

        /*  Wait till the user starts accepting once again. */
        self->state = NN_USOCK_STATE_LISTENING;

        return;

    case (NN_USOCK_STATE_CANCELLING_ACCEPT | NN_STREAM_STOPPED):
        nn_assert (srcptr == NULL);

        /*  TODO: The socket being accepted should be closed here. */

        self->state = NN_USOCK_STATE_LISTENING;

        /*  Notify the accepted socket that it was stopped. */
        nn_fsm_do_now (self->asock, NN_STREAM_ACCEPT_ERROR);

        return;

    default:
        nn_assert_unreachable_fsm (self->state, type);
        return;
    }
}