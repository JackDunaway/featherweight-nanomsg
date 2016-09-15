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

#ifndef NN_FSM_INCLUDED
#define NN_FSM_INCLUDED

#include "../utils/queue.h"

/*  Base class for state machines. */

struct nn_fsm_event {

    /*  State machine for which this event is destined. */
    struct nn_fsm *dest;

    /*  Source of the event which has a context-sensitive object type (i.e.,
        it does not necessarily need to be another FSM, and it can even be
        NULL.) This object type may be wrapped by another object type, in which
        case the event can effectively carry a payload. */
    //void *srcptr;

    /*  This event type is the first clue to the destination FSM how to
        interpret and process the event. This, combined with the current finite
        state of the destination FSM, comprises all the actions that an FSM
        handler performs. */
    int type;

    /*  Private data to the FSM object for queueing incoming events. */
    struct nn_queue_item item;
};

void nn_fsm_event_init (struct nn_fsm_event *self);
void nn_fsm_event_term (struct nn_fsm_event *self);
int nn_fsm_event_active (struct nn_fsm_event *self);
void nn_fsm_event_process (struct nn_fsm_event *self);

/*  Actions common to all state machines sent to FSM handlers. */
#define NN_FSM_START 0x00010000
#define NN_FSM_STOP 0x00020000

/*  Macro to define finite states and associated jobs that are valid for the
    given state space. */
//#define NN_FSM_JOB(mystate, jobtype) \
//   if (self->state == (mystate) && type == (jobtype))

/*  Virtual function to be implemented by the derived class to handle the
    incoming events. */
typedef void (*nn_fsm_fn) (struct nn_fsm *self, int type, void *srcptr);

struct nn_fsm {
    nn_fsm_fn handler;
    nn_fsm_fn shutdown_fn;
    int state;
    //void *srcptr;
    struct nn_fsm *owner;
    struct nn_ctx *ctx;
    struct nn_fsm_event stopped;
};

void nn_fsm_init_root (struct nn_fsm *self, nn_fsm_fn handler,
    nn_fsm_fn shutdown_fn, struct nn_ctx *ctx);
void nn_fsm_init (struct nn_fsm *self, nn_fsm_fn handler,
    nn_fsm_fn shutdown_fn, void *srcptr, struct nn_fsm *owner);
void nn_fsm_term (struct nn_fsm *self);
void nn_fsm_term_early (struct nn_fsm *self);

int nn_fsm_isidle (struct nn_fsm *self);
void nn_fsm_start (struct nn_fsm *self);
void nn_fsm_stop (struct nn_fsm *self);
void nn_fsm_stopped (struct nn_fsm *self, int type);
void nn_fsm_stopped_noevent (struct nn_fsm *self);

/*  Replaces current owner of the fsm with a new owner. */
void nn_fsm_swap_owner (struct nn_fsm *self, struct nn_fsm *newowner);

/*  Immediately perform an action on a state machine handler. Nominally, this
    is only performed by a state machine on itself. */
void nn_fsm_do_now (struct nn_fsm *self, int type);

/*  Send event from the state machine to its owner. */
void nn_fsm_raise (struct nn_fsm *self, struct nn_fsm_event *event, int type);


/*  Send event to the specified state machine. It's caller's responsibility
    to ensure that the destination state machine will still exist when the
    event is delivered.
    NOTE: This function is a hack to make inproc transport work in the most
    efficient manner. Do not use it outside of inproc transport! */
void nn_fsm_raiseto (struct nn_fsm *self, struct nn_fsm *dst,
    struct nn_fsm_event *event, int type, void *srcptr);

/*  This function provides low-level action feeding. Used in worker threads and
    timers, it shouldn't be used by others. Consider nn_fsm_do_now or
    nn_fsm_raise instead. */
void nn_fsm_feed (struct nn_fsm *self, int type, void *srcptr);

#endif
