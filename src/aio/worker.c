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

#include "worker.h"

#include "ctx.h"

#include "../utils/err.h"
#include "../utils/cont.h"

#if defined NN_HAVE_WINDOWS

#include "ctx.h"
#include "usock.h"

#define NN_WORKER_MAX_EVENTS 32

#define NN_WORKER_OP_STATE_IDLE 1
#define NN_WORKER_OP_STATE_ACTIVE 2
#define NN_WORKER_OP_STATE_ACTIVE_ZEROISERROR 3

/*  The value of this variable is irrelevant. It's used only as a placeholder
for the address that is used as the 'stop' event ID. */
const int nn_worker_stop = 0;

/*  Private functions. */
static void nn_worker_routine (void *arg);

void nn_worker_task_init (struct nn_worker_task *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
}

void nn_worker_task_term (struct nn_worker_task *self)
{
}

void nn_worker_op_init (struct nn_worker_op *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
    self->state = NN_WORKER_OP_STATE_IDLE;
}

void nn_worker_op_term (struct nn_worker_op *self)
{
    nn_assert_state (self, NN_WORKER_OP_STATE_IDLE);
}

void nn_worker_op_start (struct nn_worker_op *self, int zeroiserror)
{
    nn_assert_state (self, NN_WORKER_OP_STATE_IDLE);
    self->state = zeroiserror ? NN_WORKER_OP_STATE_ACTIVE_ZEROISERROR :
        NN_WORKER_OP_STATE_ACTIVE;
}

int nn_worker_op_isidle (struct nn_worker_op *self)
{
    return self->state == NN_WORKER_OP_STATE_IDLE ? 1 : 0;
}

int nn_worker_init (struct nn_worker *self)
{
    self->cp = CreateIoCompletionPort (INVALID_HANDLE_VALUE, NULL, 0, 0);
    nn_assert_win (self->cp);
    nn_timerset_init (&self->timerset);
    nn_thread_init (&self->thread, nn_worker_routine, self);

    return 0;
}

void nn_worker_term (struct nn_worker *self)
{
    BOOL brc;

    /*  Ask worker thread to terminate. */
    brc = PostQueuedCompletionStatus (self->cp, 0,
        (ULONG_PTR) &nn_worker_stop, NULL);
    nn_assert_win (brc);

    /*  Wait till worker thread terminates. */
    nn_thread_term (&self->thread);

    nn_timerset_term (&self->timerset);
    brc = CloseHandle (self->cp);
    nn_assert_win (brc);
}

void nn_worker_execute (struct nn_worker *self, struct nn_worker_task *task)
{
    BOOL brc;

    brc = PostQueuedCompletionStatus (self->cp, 0, (ULONG_PTR) task, NULL);
    nn_assert_win (brc);
}

void nn_worker_add_timer (struct nn_worker *self, int timeout,
    struct nn_worker_timer *timer)
{
    nn_timerset_add (&((struct nn_worker*) self)->timerset, timeout,
        &timer->hndl);
}

void nn_worker_rm_timer (struct nn_worker *self, struct nn_worker_timer *timer)
{
    nn_timerset_rm (&((struct nn_worker*) self)->timerset, &timer->hndl);
}

HANDLE nn_worker_getcp (struct nn_worker *self)
{
    return self->cp;
}

static void nn_worker_routine (void *arg)
{
    int rc;
    BOOL brc;
    struct nn_worker *self;
    int timeout;
    ULONG count;
    ULONG i;
    struct nn_timerset_hndl *thndl;
    struct nn_worker_timer *timer;
    struct nn_worker_task *task;
    struct nn_worker_op *op;
    OVERLAPPED_ENTRY entries [NN_WORKER_MAX_EVENTS];

    self = (struct nn_worker*) arg;

    while (1) {

        /*  Process all expired timers. */
        while (1) {
            rc = nn_timerset_event (&self->timerset, &thndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);
            timer = nn_cont (thndl, struct nn_worker_timer, hndl);
            nn_ctx_enter (timer->owner->ctx);
            nn_fsm_feed (timer->owner, -1, NN_WORKER_TIMER_TIMEOUT, timer);
            nn_ctx_leave (timer->owner->ctx);
        }

        /*  Compute the time interval till next timer expiration. */
        timeout = nn_timerset_timeout (&self->timerset);

        /*  Wait for new events and/or timeouts. */
        brc = GetQueuedCompletionStatusEx (self->cp, entries,
            NN_WORKER_MAX_EVENTS, &count, timeout < 0 ? INFINITE : timeout,
            FALSE);
        if (!brc && GetLastError () == WAIT_TIMEOUT)
            continue;
        nn_assert_win (brc);

        for (i = 0; i != count; ++i) {

            /*  Process I/O completion events. */
            if (entries [i].lpOverlapped != NULL) {
                op = nn_cont (entries [i].lpOverlapped,
                    struct nn_worker_op, olpd);

                /*  The 'Internal' field is actually an NTSTATUS. Report
                success and error. Ignore warnings and informational
                messages.*/
                rc = entries [i].Internal & 0xc0000000;
                switch (rc) {
                case 0x00000000:
                    rc = NN_WORKER_OP_DONE;
                    break;
                case 0xc0000000:
                    rc = NN_WORKER_OP_ERROR;
                    break;
                default:
                    continue;
                }

                /*  Raise the completion event. */
                nn_ctx_enter (op->owner->ctx);
                nn_assert (op->state != NN_WORKER_OP_STATE_IDLE);
                if (rc != NN_WORKER_OP_ERROR &&
                    op->state == NN_WORKER_OP_STATE_ACTIVE_ZEROISERROR &&
                    entries [i].dwNumberOfBytesTransferred == 0)
                    rc = NN_WORKER_OP_ERROR;
                op->state = NN_WORKER_OP_STATE_IDLE;
                nn_fsm_feed (op->owner, op->src, rc, op);
                nn_ctx_leave (op->owner->ctx);

                continue;
            }

            /*  Worker thread shutdown is requested. */
            if (entries [i].lpCompletionKey == (ULONG_PTR) &nn_worker_stop)
                return;

            /*  Process tasks. */
            task = (struct nn_worker_task*) entries [i].lpCompletionKey;
            nn_ctx_enter (task->owner->ctx);
            nn_fsm_feed (task->owner, task->src,
                NN_WORKER_TASK_EXECUTE, task);
            nn_ctx_leave (task->owner->ctx);
        }
    }
}
#else

#include "../utils/attr.h"
#include "../utils/queue.h"

/*  Private functions. */
static void nn_worker_routine (void *arg);

void nn_worker_fd_init (struct nn_worker_fd *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
}

void nn_worker_fd_term (NN_UNUSED struct nn_worker_fd *self)
{
}

void nn_worker_add_fd (struct nn_worker *self, int s, struct nn_worker_fd *fd)
{
    nn_poller_add (&self->poller, s, &fd->hndl);
}

void nn_worker_rm_fd (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_rm (&self->poller, &fd->hndl);
}

void nn_worker_set_in (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_set_in (&self->poller, &fd->hndl);
}

void nn_worker_reset_in (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_reset_in (&self->poller, &fd->hndl);
}

void nn_worker_set_out (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_set_out (&self->poller, &fd->hndl);
}

void nn_worker_reset_out (struct nn_worker *self, struct nn_worker_fd *fd)
{
    nn_poller_reset_out (&self->poller, &fd->hndl);
}

void nn_worker_add_timer (struct nn_worker *self, int timeout,
    struct nn_worker_timer *timer)
{
    nn_timerset_add (&self->timerset, timeout, &timer->hndl);
}

void nn_worker_rm_timer (struct nn_worker *self, struct nn_worker_timer *timer)
{
    nn_timerset_rm (&self->timerset, &timer->hndl);
}

void nn_worker_task_init (struct nn_worker_task *self, int src,
    struct nn_fsm *owner)
{
    self->src = src;
    self->owner = owner;
    nn_queue_item_init (&self->item);
}

void nn_worker_task_term (struct nn_worker_task *self)
{
    nn_queue_item_term (&self->item);
}

int nn_worker_init (struct nn_worker *self)
{
    int rc;

    rc = nn_efd_init (&self->efd);
    if (rc < 0)
        return rc;

    nn_mutex_init (&self->sync, 0);
    nn_queue_init (&self->tasks);
    nn_queue_item_init (&self->stop);
    nn_poller_init (&self->poller);
    nn_poller_add (&self->poller, nn_efd_getfd (&self->efd), &self->efd_hndl);
    nn_poller_set_in (&self->poller, &self->efd_hndl);
    nn_timerset_init (&self->timerset);
    nn_thread_init (&self->thread, nn_worker_routine, self);

    return 0;
}

void nn_worker_term (struct nn_worker *self)
{
    /*  Ask worker thread to terminate. */
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &self->stop);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);

    /*  Wait till worker thread terminates. */
    nn_thread_term (&self->thread);

    /*  Clean up. */
    nn_timerset_term (&self->timerset);
    nn_poller_term (&self->poller);
    nn_efd_term (&self->efd);
    nn_queue_item_term (&self->stop);
    nn_queue_term (&self->tasks);
    nn_mutex_term (&self->sync);
}

void nn_worker_execute (struct nn_worker *self, struct nn_worker_task *task)
{
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &task->item);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);
}

void nn_worker_cancel (struct nn_worker *self, struct nn_worker_task *task)
{
    nn_mutex_lock (&self->sync);
    nn_queue_remove (&self->tasks, &task->item);
    nn_mutex_unlock (&self->sync);
}

static void nn_worker_routine (void *arg)
{
    int rc;
    struct nn_worker *self;
    int pevent;
    struct nn_poller_hndl *phndl;
    struct nn_timerset_hndl *thndl;
    struct nn_queue tasks;
    struct nn_queue_item *item;
    struct nn_worker_task *task;
    struct nn_worker_fd *fd;
    struct nn_worker_timer *timer;

    self = (struct nn_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
    shut down. */
    while (1) {

        /*  Wait for new events and/or timeouts. */
        rc = nn_poller_wait (&self->poller,
            nn_timerset_timeout (&self->timerset));
        errnum_assert (rc == 0, -rc);

        /*  Process all expired timers. */
        while (1) {
            rc = nn_timerset_event (&self->timerset, &thndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);
            timer = nn_cont (thndl, struct nn_worker_timer, hndl);
            nn_ctx_enter (timer->owner->ctx);
            nn_fsm_feed (timer->owner, -1, NN_WORKER_TIMER_TIMEOUT, timer);
            nn_ctx_leave (timer->owner->ctx);
        }

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = nn_poller_event (&self->poller, &pevent, &phndl);
            if (nn_slow (rc == -EAGAIN))
                break;

            /*  If there are any new incoming worker tasks, process them. */
            if (phndl == &self->efd_hndl) {
                nn_assert (pevent == NN_POLLER_IN);

                /*  Make a local copy of the task queue. This way
                the application threads are not blocked and can post new
                tasks while the existing tasks are being processed. Also,
                new tasks can be posted from within task handlers. */
                nn_mutex_lock (&self->sync);
                nn_efd_unsignal (&self->efd);
                memcpy (&tasks, &self->tasks, sizeof (tasks));
                nn_queue_init (&self->tasks);
                nn_mutex_unlock (&self->sync);

                while (1) {

                    /*  Next worker task. */
                    item = nn_queue_pop (&tasks);
                    if (nn_slow (!item))
                        break;

                    /*  If the worker thread is asked to stop, do so. */
                    if (nn_slow (item == &self->stop)) {
                        /*  Make sure we remove all the other workers from
                        the queue, because we're not doing anything with
                        them. */
                        while (nn_queue_pop (&tasks) != NULL) {
                            continue;
                        }
                        nn_queue_term (&tasks);
                        return;
                    }

                    /*  It's a user-defined task. Notify the user that it has
                    arrived in the worker thread. */
                    task = nn_cont (item, struct nn_worker_task, item);
                    nn_ctx_enter (task->owner->ctx);
                    nn_fsm_feed (task->owner, task->src,
                        NN_WORKER_TASK_EXECUTE, task);
                    nn_ctx_leave (task->owner->ctx);
                }
                nn_queue_term (&tasks);
                continue;
            }

            /*  It's a true I/O event. Invoke the handler. */
            fd = nn_cont (phndl, struct nn_worker_fd, hndl);
            nn_ctx_enter (fd->owner->ctx);
            nn_fsm_feed (fd->owner, fd->src, pevent, fd);
            nn_ctx_leave (fd->owner->ctx);
        }
    }
}
#endif

void nn_worker_timer_init (struct nn_worker_timer *self, struct nn_fsm *owner)
{
    self->owner = owner;
    nn_timerset_hndl_init (&self->hndl);
}

void nn_worker_timer_term (struct nn_worker_timer *self)
{
    nn_timerset_hndl_term (&self->hndl);
}

int nn_worker_timer_isactive (struct nn_worker_timer *self)
{
    return nn_timerset_hndl_isactive (&self->hndl);
}
