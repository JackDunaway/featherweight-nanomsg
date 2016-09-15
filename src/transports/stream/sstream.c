/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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

#include "sstream.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/wire.h"
#include "../../utils/attr.h"

/*  Time allowed to complete opening handshake. */
#ifndef NN_SSTREAM_STREAMHDR_TIMEOUT
#   define NN_SSTREAM_STREAMHDR_TIMEOUT 1000
#endif

/*  States of the object as a whole. */
#define NN_SSTREAM_STATE_IDLE 1
#define NN_SSTREAM_STATE_STREAMHDR_SENDING 2
#define NN_SSTREAM_STATE_STREAMHDR_RECEIVING 3
#define NN_SSTREAM_STATE_STREAMHDR_ERROR 4
#define NN_SSTREAM_STATE_STREAMHDR_SUCCESS 5
#define NN_SSTREAM_STATE_ACTIVE 6
#define NN_SSTREAM_STATE_SHUTTING_DOWN 7
#define NN_SSTREAM_STATE_DONE 8
#define NN_SSTREAM_STATE_STOPPING_TIMER 9

/*  Possible states of the inbound part of the object. */
#define NN_SSTREAM_INSTATE_IDLE 1
#define NN_SSTREAM_INSTATE_HDR 2
#define NN_SSTREAM_INSTATE_BODY 3
#define NN_SSTREAM_INSTATE_HASMSG 4

/*  Possible states of the outbound part of the object. */
#define NN_SSTREAM_OUTSTATE_IDLE 1
#define NN_SSTREAM_OUTSTATE_READY 2
#define NN_SSTREAM_OUTSTATE_SENDING 3

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int nn_sstream_send (struct nn_pipebase *self, struct nn_msg *msg);
static int nn_sstream_recv (struct nn_pipebase *self, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_sstream_pipebase_vfptr = {
    nn_sstream_send,
    nn_sstream_recv
};

/*  Private functions. */
static void nn_sstream_handler (struct nn_fsm *self, int type, void *srcptr);
static void nn_sstream_shutdown (struct nn_fsm *self, int type, void *srcptr);

void nn_sstream_init (struct nn_sstream *self,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_sstream_handler, nn_sstream_shutdown,
        self, owner);
    self->state = NN_SSTREAM_STATE_IDLE;
    nn_timer_init (&self->timer, self->usock->worker, &self->fsm);
    self->usock = NULL;
    self->owner = NULL;
    nn_pipebase_init (&self->pipebase, &nn_sstream_pipebase_vfptr, epbase);
    self->instate = NN_SSTREAM_INSTATE_IDLE;
    nn_msg_init (&self->inmsg, 0);
    self->outstate = NN_SSTREAM_OUTSTATE_IDLE;
    nn_msg_init (&self->outmsg, 0);
    nn_fsm_event_init (&self->done);
}

void nn_sstream_term (struct nn_sstream *self)
{
    nn_assert_state (self, NN_SSTREAM_STATE_IDLE);

    nn_fsm_event_term (&self->done);
    nn_msg_term (&self->outmsg);
    nn_msg_term (&self->inmsg);
    nn_pipebase_term (&self->pipebase);
    nn_timer_term (&self->timer);
    nn_fsm_term (&self->fsm);
}

int nn_sstream_isidle (struct nn_sstream *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_sstream_start (struct nn_sstream *self, struct nn_stream *usock)
{
    size_t sz;
    int protocol;

    /*  Take ownership of the underlying socket. */
    nn_assert (self->usock == NULL && self->owner == NULL);
    nn_stream_swap_owner (usock, &self->fsm);
    self->usock = usock;

    /*  Get the protocol identifier. */
    sz = sizeof (protocol);
    nn_pipebase_getopt (&self->pipebase, NN_SOL_SOCKET, NN_PROTOCOL,
        &protocol, &sz);
    nn_assert (sz == sizeof (protocol));

    /*  Compose the protocol header. */
    memcpy (self->protohdr, "\0SP\0\0\0\0\0", 8);
    nn_puts (self->protohdr + 4, (uint16_t) protocol);

    /*  Launch the state machine. */
    nn_fsm_start (&self->fsm);
}

void nn_sstream_stop (struct nn_sstream *self)
{
    nn_fsm_stop (&self->fsm);
}

static int nn_sstream_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_sstream *sstream;
    struct nn_iovec iov [3];

    sstream = nn_cont (self, struct nn_sstream, pipebase);

    nn_assert_state (sstream, NN_SSTREAM_STATE_ACTIVE);
    nn_assert (sstream->outstate == NN_SSTREAM_OUTSTATE_READY);

    /*  Move the message to the local storage. */
    nn_msg_term (&sstream->outmsg);
    nn_msg_mv (&sstream->outmsg, msg);

    /*  Serialise the message header. */
    sstream->outhdr [0] = NN_SIPC_MSG_NORMAL;
    nn_putll (sstream->outhdr + 1, nn_chunkref_size (&sstream->outmsg.sphdr) +
        nn_chunkref_size (&sstream->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = sstream->outhdr;
    iov [0].iov_len = sizeof (sstream->outhdr);
    iov [1].iov_base = nn_chunkref_data (&sstream->outmsg.sphdr);
    iov [1].iov_len = nn_chunkref_size (&sstream->outmsg.sphdr);
    iov [2].iov_base = nn_chunkref_data (&sstream->outmsg.body);
    iov [2].iov_len = nn_chunkref_size (&sstream->outmsg.body);
    nn_stream_send (sstream->usock, iov, 3);

    sstream->outstate = NN_SSTREAM_OUTSTATE_SENDING;

    return 0;
}

static int nn_sstream_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_sstream *sstream;

    sstream = nn_cont (self, struct nn_sstream, pipebase);

    nn_assert_state (sstream, NN_SSTREAM_STATE_ACTIVE);
    nn_assert (sstream->instate == NN_SSTREAM_INSTATE_HASMSG);

    /*  Move received message to the user. */
    nn_msg_mv (msg, &sstream->inmsg);
    nn_msg_init (&sstream->inmsg, 0);

    /*  Start receiving new message. */
    sstream->instate = NN_SSTREAM_INSTATE_HDR;
    nn_stream_recv (sstream->usock, sstream->inhdr, sizeof (sstream->inhdr));

    return 0;
}

static void nn_sstream_shutdown (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_sstream *sstream;

    sstream = nn_cont (self, struct nn_sstream, fsm);
    nn_assert (srcptr == NULL);

    if (type == NN_FSM_STOP) {
        nn_pipebase_stop (&sstream->pipebase);
        nn_timer_cancel (&sstream->timer);
        sstream->state = NN_SSTREAM_STATE_STOPPING_TIMER;
    }
    if (sstream->state == NN_SSTREAM_STATE_STOPPING_TIMER) {
        if (nn_timer_isidle (&sstream->timer)) {
            nn_stream_swap_owner (sstream->usock, sstream->owner);
            sstream->usock = NULL;
            sstream->owner = NULL;
            sstream->state = NN_SSTREAM_STATE_IDLE;
            nn_fsm_stopped (&sstream->fsm, NN_SSTREAM_STOPPED);
            return;
        }
        return;
    }

    nn_assert_unreachable_fsm (sstream->state, type);
}

static void nn_sstream_handler (struct nn_fsm *self, int type, void *srcptr)
{
    struct nn_sstream *sstream;
    struct nn_iovec iovec;
    int protocol;
    uint64_t size;
    int rc;
    int opt;
    size_t opt_sz = sizeof (opt);

    sstream = nn_cont (self, struct nn_sstream, fsm);
    nn_assert (srcptr == NULL);

    switch (sstream->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_SSTREAM_STATE_IDLE:
        switch (type) {
        case NN_FSM_START:
            nn_timer_start (&sstream->timer, NN_STREAM_HANDSHAKE_TIMEDOUT,
                NN_SSTREAM_STREAMHDR_TIMEOUT);
            iovec.iov_base = sstream->protohdr;
            iovec.iov_len = sizeof (sstream->protohdr);
            nn_stream_send (sstream->usock, &iovec, 1);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_SENDING;
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

/******************************************************************************/
/*  STREAMHDR_SENDING state.                                                  */
/******************************************************************************/
    case NN_SSTREAM_STATE_STREAMHDR_SENDING:
        switch (type) {
        case NN_STREAM_SENT:
            nn_stream_recv (sstream->usock, sstream->protohdr,
                sizeof (sstream->protohdr));
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_RECEIVING;
            return;
        case NN_STREAM_SHUTDOWN:
            /*  Ignore it. Wait for ERROR event  */
            return;
        case NN_STREAM_ERROR:
            nn_timer_cancel (&sstream->timer);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
            return;
        case NN_STREAM_HANDSHAKE_TIMEDOUT:
            nn_timer_cancel (&sstream->timer);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

    case NN_SSTREAM_STATE_STREAMHDR_RECEIVING:
        switch (type) {
        case NN_STREAM_RECEIVED:

            /*  Here we are checking whether the peer speaks the same
            protocol as this socket. */
            if (memcmp (sstream->protohdr, "\0SP\0", 4) != 0) {
                nn_timer_cancel (&sstream->timer);
                sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
                return;
            }
            protocol = nn_gets (sstream->protohdr + 4);
            if (!nn_pipebase_ispeer (&sstream->pipebase, protocol)) {
                nn_timer_cancel (&sstream->timer);
                sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
                return;
            }
            nn_timer_cancel (&sstream->timer);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_SUCCESS;
            return;
        case NN_STREAM_SHUTDOWN:
            /*  Ignore it. Wait for ERROR event  */
            return;
        case NN_STREAM_ERROR:
            nn_timer_cancel (&sstream->timer);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
            return;
        case NN_STREAM_HANDSHAKE_TIMEDOUT:
            nn_timer_cancel (&sstream->timer);
            sstream->state = NN_SSTREAM_STATE_STREAMHDR_ERROR;
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

/******************************************************************************/
/*  STREAMHDR_ERROR state.                                                    */
/******************************************************************************/
    case NN_SSTREAM_STATE_STREAMHDR_ERROR:
        switch (type) {
        case NN_STREAM_ERROR:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            return;
        case NN_EVENT_TIMER_STOPPED:
            /*  Raise the error and move directly to the DONE state. */
            sstream->state = NN_SSTREAM_STATE_DONE;
            nn_fsm_raise (&sstream->fsm, &sstream->done, NN_SSTREAM_ERROR);
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

/******************************************************************************/
/*  STREAMHDR_SUCCESS state.                                                  */
/******************************************************************************/
    case NN_SSTREAM_STATE_STREAMHDR_SUCCESS:
        switch (type) {

        case NN_STREAM_ERROR:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            return;
        case NN_EVENT_TIMER_STOPPED:
            /*  Start the pipe. */
            rc = nn_pipebase_start (&sstream->pipebase);
            if (rc < 0) {
                sstream->state = NN_SSTREAM_STATE_DONE;
                nn_fsm_raise (&sstream->fsm, &sstream->done, NN_SSTREAM_ERROR);
                return;
            }

            /*  Start receiving a message in asynchronous manner. */
            sstream->instate = NN_SSTREAM_INSTATE_HDR;
            nn_stream_recv (sstream->usock, &sstream->inhdr,
                sizeof (sstream->inhdr));

            /*  Mark the pipe as available for sending. */
            sstream->outstate = NN_SSTREAM_OUTSTATE_READY;

            sstream->state = NN_SSTREAM_STATE_ACTIVE;
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case NN_SSTREAM_STATE_ACTIVE:
        switch (type) {
        case NN_STREAM_SENT:

            /*  The message is now fully sent. */
            nn_assert (sstream->outstate == NN_SSTREAM_OUTSTATE_SENDING);
            sstream->outstate = NN_SSTREAM_OUTSTATE_IDLE;
            nn_msg_term (&sstream->outmsg);
            nn_msg_init (&sstream->outmsg, 0);
            nn_pipebase_sent (&sstream->pipebase);
            return;

        case NN_STREAM_RECEIVED:

            switch (sstream->instate) {
            case NN_SSTREAM_INSTATE_HDR:

                /*  Message header was received. Check that message size
                    is acceptable by comparing with NN_RCVMAXSIZE;
                    if it's too large, drop the connection. */
                nn_assert (sstream->inhdr [0] == NN_SIPC_MSG_NORMAL);
                size = nn_getll (sstream->inhdr + 1);

                nn_pipebase_getopt (&sstream->pipebase, NN_SOL_SOCKET,
                    NN_RCVMAXSIZE, &opt, &opt_sz);

                if (opt >= 0 && size > (unsigned) opt) {
                    sstream->state = NN_SSTREAM_STATE_DONE;
                    nn_fsm_raise (&sstream->fsm, &sstream->done, NN_SSTREAM_ERROR);
                    return;
                }

                /*  Allocate memory for the message. */
                nn_msg_term (&sstream->inmsg);
                nn_msg_init (&sstream->inmsg, (size_t) size);

                /*  Special case when size of the message body is 0. */
                if (!size) {
                    sstream->instate = NN_SSTREAM_INSTATE_HASMSG;
                    nn_pipebase_received (&sstream->pipebase);
                    return;
                }

                /*  Start receiving the message body. */
                sstream->instate = NN_SSTREAM_INSTATE_BODY;
                nn_stream_recv (sstream->usock,
                    nn_chunkref_data (&sstream->inmsg.body),
                    (size_t) size);

                return;

            case NN_SSTREAM_INSTATE_BODY:

                /*  Message body was received. Notify the owner that it
                    can receive it. */
                sstream->instate = NN_SSTREAM_INSTATE_HASMSG;
                nn_pipebase_received (&sstream->pipebase);

                return;

            default:
                nn_assert_unreachable ("Unexpected [instate] value.");
                return;
            }

        case NN_STREAM_SHUTDOWN:
            nn_pipebase_stop (&sstream->pipebase);
            sstream->state = NN_SSTREAM_STATE_SHUTTING_DOWN;
            return;

        case NN_STREAM_ERROR:
            nn_pipebase_stop (&sstream->pipebase);
            sstream->state = NN_SSTREAM_STATE_DONE;
            nn_fsm_raise (&sstream->fsm, &sstream->done, NN_SSTREAM_ERROR);
            return;


        default:
            nn_assert_unreachable_fsm (sstream->state, type);
            return;
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case NN_SSTREAM_STATE_SHUTTING_DOWN:
        switch (type) {
        case NN_STREAM_ERROR:
            sstream->state = NN_SSTREAM_STATE_DONE;
            nn_fsm_raise (&sstream->fsm, &sstream->done, NN_SSTREAM_ERROR);
            return;
        default:
            nn_assert_unreachable_fsm (sstream->state, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case NN_SSTREAM_STATE_DONE:
        nn_assert_unreachable_fsm (sstream->state, type);


/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_assert_unreachable_fsm (sstream->state, type);
    }
}
