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

#include "ctx.h"

#include "../utils/err.h"
#include "../utils/cont.h"

void nn_ctx_init (struct nn_ctx *self, struct nn_pool *pool,
    nn_ctx_onleave onleave, int initial_holds)
{
    nn_mutex_init (&self->sync, 1);
    self->locks = 0;
    self->holds = initial_holds;
    self->pool = pool;
    nn_queue_init (&self->incoming);
    nn_queue_init (&self->outgoing);
    nn_sem_init (&self->released);
    self->onleave = onleave;
}

void nn_ctx_term (struct nn_ctx *self)
{
    nn_assert (self->holds == 0);
    nn_assert (self->locks == 0);
    nn_queue_term (&self->outgoing);
    nn_queue_term (&self->incoming);
    nn_mutex_term (&self->sync);
}

void nn_ctx_enter (struct nn_ctx *self)
{
    nn_mutex_lock (&self->sync);
    ++self->locks;
}

void nn_ctx_leave (struct nn_ctx *self)
{
    struct nn_queue_item *item;
    struct nn_fsm_event *event;
    struct nn_ctx *destctx;

    /*  Process any queued events before leaving the context. */
    while (1) {
        item = nn_queue_pop (&self->incoming);
        if (!item) {
            break;
        }
        event = nn_cont (item, struct nn_fsm_event, item);
        nn_fsm_event_process (event);
    }

    /*  Process all queued external events while holding the exclusive
        lock on its owning context. */
    while (1) {
        item = nn_queue_pop (&self->outgoing);
        if (!item) {
            break;
        }
        event = nn_cont (item, struct nn_fsm_event, item);
        destctx = event->dest->ctx;
        nn_ctx_enter (destctx);
        nn_fsm_event_process (event);
        nn_ctx_leave (destctx);
    }

    /*  Notify the owner that we are leaving the context. */
    if (self->onleave) {
        self->onleave (self);
    }

    --self->locks;
    nn_mutex_unlock (&self->sync);
}

int nn_ctx_hold (struct nn_ctx *self)
{
    /*  A hold may only gained while also holding a lock. */
    nn_assert (self->locks > 0);
    self->holds++;

    return 0;
}

void nn_ctx_release (struct nn_ctx *self)
{
    nn_assert (self->holds > 0);
    self->holds--;
    if (self->holds == 0) {
        nn_sem_post (&self->released);
    }
}

void nn_ctx_wait_til_released (struct nn_ctx *self)
{
    nn_sem_wait (&self->released);
    nn_mutex_lock (&self->sync);
    nn_assert (self->holds == 0);
    nn_assert (nn_queue_empty (&self->incoming));
    nn_assert (nn_queue_empty (&self->outgoing));
    nn_mutex_unlock (&self->sync);
}

void nn_ctx_raise (struct nn_ctx *self, struct nn_fsm_event *event)
{
    nn_queue_push (&self->incoming, &event->item);
}

void nn_ctx_raiseto (struct nn_ctx *self, struct nn_fsm_event *event)
{
    nn_queue_push (&self->outgoing, &event->item);
}

