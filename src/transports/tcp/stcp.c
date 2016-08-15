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

#include "stcp.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/wire.h"
#include "../../utils/attr.h"

/*  States of the object as a whole. */
#define NN_STCP_STATE_IDLE 1
#define NN_STCP_STATE_STREAMHDR_SENDING 2
#define NN_STCP_STATE_STREAMHDR_RECVING 3
#define NN_STCP_STATE_STREAMHDR_ERROR 4
#define NN_STCP_STATE_STREAMHDR_SUCCESS 5
#define NN_STCP_STATE_ACTIVE 6
#define NN_STCP_STATE_SHUTTING_DOWN 7
#define NN_STCP_STATE_DONE 8
#define NN_STCP_STATE_STOPPING_TIMER 9

/*  Subordinate srcptr objects. */
#define NN_STCP_SRC_USOCK 1
#define NN_STCP_SRC_TIMER 2

/*  Possible states of the inbound part of the object. */
#define NN_STCP_INSTATE_HDR 1
#define NN_STCP_INSTATE_BODY 2
#define NN_STCP_INSTATE_HASMSG 3

/*  Time allowed to complete opening handshake. */
#ifndef NN_STCP_STREAMHDR_TIMEOUT
#   define NN_STCP_STREAMHDR_TIMEOUT 1000
#endif

/*  Possible states of the outbound part of the object. */
#define NN_STCP_OUTSTATE_IDLE 1
#define NN_STCP_OUTSTATE_SENDING 2

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int nn_stcp_send (struct nn_pipebase *self, struct nn_msg *msg);
static int nn_stcp_recv (struct nn_pipebase *self, struct nn_msg *msg);
const struct nn_pipebase_vfptr nn_stcp_pipebase_vfptr = {
    nn_stcp_send,
    nn_stcp_recv
};

/*  Private functions. */
static void nn_stcp_handler (struct nn_fsm *self, int src, int type,
    void *srcptr);
static void nn_stcp_shutdown (struct nn_fsm *self, int src, int type,
    void *srcptr);

void nn_stcp_init (struct nn_stcp *self, int src,
    struct nn_epbase *epbase, struct nn_fsm *owner)
{
    nn_fsm_init (&self->fsm, nn_stcp_handler, nn_stcp_shutdown,
        src, self, owner);
    self->state = NN_STCP_STATE_IDLE;
    nn_timer_init (&self->timer, NN_STCP_SRC_TIMER, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    nn_pipebase_init (&self->pipebase, &nn_stcp_pipebase_vfptr, epbase);
    self->instate = -1;
    nn_msg_init (&self->inmsg, 0);
    self->outstate = -1;
    nn_msg_init (&self->outmsg, 0);
    nn_fsm_event_init (&self->done);
}

void nn_stcp_term (struct nn_stcp *self)
{
    nn_assert_state (self, NN_STCP_STATE_IDLE);

    nn_fsm_event_term (&self->done);
    nn_msg_term (&self->outmsg);
    nn_msg_term (&self->inmsg);
    nn_pipebase_term (&self->pipebase);
    nn_timer_term (&self->timer);
    nn_fsm_term (&self->fsm);
}

int nn_stcp_isidle (struct nn_stcp *self)
{
    return nn_fsm_isidle (&self->fsm);
}

void nn_stcp_start (struct nn_stcp *self, struct nn_utcp *usock)
{
    size_t sz;
    int protocol;

    /*  Take ownership of the underlying socket. */
    nn_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = NN_STCP_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    nn_utcp_swap_owner (usock, &self->usock_owner);
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

void nn_stcp_stop (struct nn_stcp *self)
{
    nn_fsm_stop (&self->fsm);
}

static int nn_stcp_send (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_stcp *stcp;
    struct nn_iovec iov [3];

    stcp = nn_cont (self, struct nn_stcp, pipebase);

    nn_assert_state (stcp, NN_STCP_STATE_ACTIVE);
    nn_assert (stcp->outstate == NN_STCP_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    nn_msg_term (&stcp->outmsg);
    nn_msg_mv (&stcp->outmsg, msg);

    /*  Serialise the message header. */
    nn_putll (stcp->outhdr, nn_chunkref_size (&stcp->outmsg.sphdr) +
        nn_chunkref_size (&stcp->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = stcp->outhdr;
    iov [0].iov_len = sizeof (stcp->outhdr);
    iov [1].iov_base = nn_chunkref_data (&stcp->outmsg.sphdr);
    iov [1].iov_len = nn_chunkref_size (&stcp->outmsg.sphdr);
    iov [2].iov_base = nn_chunkref_data (&stcp->outmsg.body);
    iov [2].iov_len = nn_chunkref_size (&stcp->outmsg.body);
    nn_utcp_send (stcp->usock, iov, 3);

    stcp->outstate = NN_STCP_OUTSTATE_SENDING;

    return 0;
}

static int nn_stcp_recv (struct nn_pipebase *self, struct nn_msg *msg)
{
    struct nn_stcp *stcp;

    stcp = nn_cont (self, struct nn_stcp, pipebase);

    nn_assert_state (stcp, NN_STCP_STATE_ACTIVE);
    nn_assert (stcp->instate == NN_STCP_INSTATE_HASMSG);

    /*  Move received message to the user. */
    nn_msg_mv (msg, &stcp->inmsg);
    nn_msg_init (&stcp->inmsg, 0);

    /*  Start receiving new message. */
    stcp->instate = NN_STCP_INSTATE_HDR;
    nn_utcp_recv (stcp->usock, stcp->inhdr, sizeof (stcp->inhdr));

    return 0;
}

static void nn_stcp_shutdown (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_stcp *stcp;

    stcp = nn_cont (self, struct nn_stcp, fsm);

    if (src == NN_FSM_ACTION && type == NN_FSM_STOP) {
        nn_pipebase_stop (&stcp->pipebase);
        nn_timer_stop (&stcp->timer);
        stcp->state = NN_STCP_STATE_STOPPING_TIMER;
    }
    if (stcp->state == NN_STCP_STATE_STOPPING_TIMER) {
        if (nn_timer_isidle (&stcp->timer)) {
            nn_utcp_swap_owner (stcp->usock, &stcp->usock_owner);
            stcp->usock = NULL;
            stcp->usock_owner.src = -1;
            stcp->usock_owner.fsm = NULL;
            stcp->state = NN_STCP_STATE_IDLE;
            nn_fsm_stopped (&stcp->fsm, NN_STCP_STOPPED);
            return;
        }
        return;
    }

    nn_fsm_bad_state(stcp->state, src, type);
}

static void nn_stcp_handler (struct nn_fsm *self, int src, int type,
    NN_UNUSED void *srcptr)
{
    struct nn_stcp *stcp;
    struct nn_iovec iovec;
    int protocol;
    uint64_t size;
    int rc;
    int opt;
    size_t opt_sz = sizeof (opt);

    stcp = nn_cont (self, struct nn_stcp, fsm);

    switch (stcp->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case NN_STCP_STATE_IDLE:
        switch (src) {

        case NN_FSM_ACTION:
            switch (type) {
            case NN_FSM_START:
                nn_timer_start (&stcp->timer, NN_STCP_STREAMHDR_TIMEOUT);
                iovec.iov_base = stcp->protohdr;
                iovec.iov_len = sizeof (stcp->protohdr);
                nn_utcp_send (stcp->usock, &iovec, 1);
                stcp->state = NN_STCP_STATE_STREAMHDR_SENDING;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  STREAMHDR_SENDING state.                                                  */
/******************************************************************************/
    case NN_STCP_STATE_STREAMHDR_SENDING:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            switch (type) {
            case NN_STREAM_SENT:
                nn_utcp_recv (stcp->usock, stcp->protohdr,
                    sizeof (stcp->protohdr));
                stcp->state = NN_STCP_STATE_STREAMHDR_RECVING;
                return;
            case NN_STREAM_SHUTDOWN:
                /*  Ignore it. Wait for ERROR event  */
                return;
            case NN_STREAM_ERROR:
                nn_timer_stop (&stcp->timer);
                stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        case NN_STCP_SRC_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:
                nn_timer_stop (&stcp->timer);
                stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  STREAMHDR_RECVING state.                                                  */
/******************************************************************************/
    case NN_STCP_STATE_STREAMHDR_RECVING:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            switch (type) {
            case NN_STREAM_RECEIVED:

                /*  Here we are checking whether the peer speaks the same
                    protocol as this socket. */
                if (memcmp (stcp->protohdr, "\0SP\0", 4) != 0) {
                    nn_timer_stop (&stcp->timer);
                    stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                    return;
                }
                protocol = nn_gets (stcp->protohdr + 4);
                if (!nn_pipebase_ispeer (&stcp->pipebase, protocol)) {
                    nn_timer_stop (&stcp->timer);
                    stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                    return;
                }
                nn_timer_stop (&stcp->timer);
                stcp->state = NN_STCP_STATE_STREAMHDR_SUCCESS;
                return;
            case NN_STREAM_SHUTDOWN:
                /*  Ignore it. Wait for ERROR event  */
                return;
            case NN_STREAM_ERROR:
                nn_timer_stop (&stcp->timer);
                stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        case NN_STCP_SRC_TIMER:
            switch (type) {
            case NN_TIMER_TIMEOUT:
                nn_timer_stop (&stcp->timer);
                stcp->state = NN_STCP_STATE_STREAMHDR_ERROR;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  STREAMHDR_ERROR state.                                                    */
/******************************************************************************/
    case NN_STCP_STATE_STREAMHDR_ERROR:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            nn_assert (type == NN_STREAM_ERROR);
            return;

        case NN_STCP_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:
                /*  Raise the error and move directly to the DONE state. */
                stcp->state = NN_STCP_STATE_DONE;
                nn_fsm_raise (&stcp->fsm, &stcp->done, NN_STCP_ERROR);
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  STREAMHDR_SUCCESS state.                                                  */
/******************************************************************************/
    case NN_STCP_STATE_STREAMHDR_SUCCESS:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            nn_assert (type == NN_STREAM_ERROR);
            return;

        case NN_STCP_SRC_TIMER:
            switch (type) {
            case NN_TIMER_STOPPED:

                /*  Start the pipe. */
                rc = nn_pipebase_start (&stcp->pipebase);
                if (rc < 0) {
                    stcp->state = NN_STCP_STATE_DONE;
                    nn_fsm_raise (&stcp->fsm, &stcp->done, NN_STCP_ERROR);
                    return;
                }

                /*  Start receiving a message in asynchronous manner. */
                stcp->instate = NN_STCP_INSTATE_HDR;
                nn_utcp_recv (stcp->usock, &stcp->inhdr,
                    sizeof (stcp->inhdr));

                /*  Mark the pipe as available for sending. */
                stcp->outstate = NN_STCP_OUTSTATE_IDLE;

                stcp->state = NN_STCP_STATE_ACTIVE;
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case NN_STCP_STATE_ACTIVE:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            switch (type) {
            case NN_STREAM_SENT:

                /*  The message is now fully sent. */
                nn_assert (stcp->outstate == NN_STCP_OUTSTATE_SENDING);
                stcp->outstate = NN_STCP_OUTSTATE_IDLE;
                nn_msg_term (&stcp->outmsg);
                nn_msg_init (&stcp->outmsg, 0);
                nn_pipebase_sent (&stcp->pipebase);
                return;

            case NN_STREAM_RECEIVED:

                switch (stcp->instate) {
                case NN_STCP_INSTATE_HDR:

                    /*  Message header was received. Check that message size
                        is acceptable by comparing with NN_RCVMAXSIZE;
                        if it's too large, drop the connection. */
                    size = nn_getll (stcp->inhdr);

                    nn_pipebase_getopt (&stcp->pipebase, NN_SOL_SOCKET,
                        NN_RCVMAXSIZE, &opt, &opt_sz);

                    if (opt >= 0 && size > (unsigned)opt) {
                        stcp->state = NN_STCP_STATE_DONE;
                        nn_fsm_raise (&stcp->fsm, &stcp->done, NN_STCP_ERROR);
                        return;
                    }

                    /*  Allocate memory for the message. */
                    nn_msg_term (&stcp->inmsg);
                    nn_msg_init (&stcp->inmsg, (size_t) size);

                    /*  Special case when size of the message body is 0. */
                    if (!size) {
                        stcp->instate = NN_STCP_INSTATE_HASMSG;
                        nn_pipebase_received (&stcp->pipebase);
                        return;
                    }

                    /*  Start receiving the message body. */
                    stcp->instate = NN_STCP_INSTATE_BODY;
                    nn_utcp_recv (stcp->usock,
                        nn_chunkref_data (&stcp->inmsg.body),
                        (size_t) size);

                    return;

                case NN_STCP_INSTATE_BODY:

                    /*  Message body was received. Notify the owner that it
                        can receive it. */
                    stcp->instate = NN_STCP_INSTATE_HASMSG;
                    nn_pipebase_received (&stcp->pipebase);

                    return;

                default:
                    nn_assert_unreachable ("Unexpected [instate] value.");
                }

            case NN_STREAM_SHUTDOWN:
                nn_pipebase_stop (&stcp->pipebase);
                stcp->state = NN_STCP_STATE_SHUTTING_DOWN;
                return;

            case NN_STREAM_ERROR:
                nn_pipebase_stop (&stcp->pipebase);
                stcp->state = NN_STCP_STATE_DONE;
                nn_fsm_raise (&stcp->fsm, &stcp->done, NN_STCP_ERROR);
                return;

            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case NN_STCP_STATE_SHUTTING_DOWN:
        switch (src) {

        case NN_STCP_SRC_USOCK:
            switch (type) {
            case NN_STREAM_ERROR:
                stcp->state = NN_STCP_STATE_DONE;
                nn_fsm_raise (&stcp->fsm, &stcp->done, NN_STCP_ERROR);
                return;
            default:
                nn_fsm_bad_action (stcp->state, src, type);
            }

        default:
            nn_fsm_bad_source (stcp->state, src, type);
        }


/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case NN_STCP_STATE_DONE:
        nn_fsm_bad_source (stcp->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        nn_fsm_bad_state (stcp->state, src, type);
    }
}

