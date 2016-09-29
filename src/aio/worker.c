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

#include "../utils/cont.h"
#include "../utils/err.h"

#define NN_WORKER_TASK_IO 0x00007100
#define NN_WORKER_TASK_ADDTIMER 0x00007200
#define NN_WORKER_TASK_STOP 0x00007300

/*  Private functions. */
static void nn_worker_routine (void *arg);
void nn_worker_task_feed (struct nn_worker *self, struct nn_taskbase *task);

static int nn_worker_timer_process (struct nn_worker *self)
{
    struct nn_timer *t;
    uint64_t now;

    /*  Iterate over all active timeouts. */
    while (1) {

        /*  If there are no active timers, then the timeout is -1 (infinite)
            and there are no timeout events to report. */
        if (nn_list_empty (&self->timeouts)) {
            return NN_INFINITE_TIMEOUT;
        }

        t = nn_cont (nn_list_begin (&self->timeouts), struct nn_timer, item);


        now = nn_clock_ms ();

        /*  Return number of msec into the future the next timer expires. */
        if (t->expiry > now) {
            return (int) (t->expiry - now);
        }

        /*  The timer has expired; remove it from the list of active timers
            and report the timeout notification. */
        nn_list_erase (&self->timeouts, &t->item);
        nn_ctx_enter (t->complete.dest->ctx);
        nn_fsm_feed (t->complete.dest, t->complete.type, t);
        nn_ctx_leave (t->complete.dest->ctx);
    }
}

static int nn_worker_timer_add (struct nn_worker *self, struct nn_timer *timer)
{
    struct nn_timer *itt;
    struct nn_list_item *it;
    int first;

    /*  Compute the instant when the timeout will be due. */
    timer->expiry = nn_clock_ms () + timer->timeout;

    /*  Insert it into the ordered list of timeouts. */
    for (it = nn_list_begin (&self->timeouts);
        it != nn_list_end (&self->timeouts);
        it = nn_list_next (&self->timeouts, it)) {
        itt = nn_cont (it, struct nn_timer, item);
        if (timer->expiry < itt->expiry)
            break;
    }

    /*  If the new timeout happens to be the first one to expire, let the user
        know that the current waiting interval has to be changed. */
    first = (nn_list_begin (&self->timeouts) == it) ? 1 : 0;
    nn_list_insert (&self->timeouts, &timer->item, it);

    return first;
}

#if defined NN_HAVE_WINDOWS

#define NN_WORKER_MAX_EVENTS 32

void nn_worker_task_feed (struct nn_worker *self, struct nn_taskbase *task)
{
    BOOL brc;

    brc = PostQueuedCompletionStatus (self->cp, 0, (ULONG_PTR) task, NULL);
    nn_assert_win (brc);
}

int nn_worker_init (struct nn_worker *self)
{
    self->cp = CreateIoCompletionPort (INVALID_HANDLE_VALUE, NULL, 0, 0);
    nn_assert_win (self->cp);
    nn_list_init (&self->timeouts);
    nn_thread_init (&self->thread, nn_worker_routine, self);

    return 0;
}

void nn_worker_term (struct nn_worker *self)
{
    struct nn_taskbase stop;
    BOOL brc;

    stop.internaltype = NN_WORKER_TASK_STOP;

    /*  Ask worker thread to terminate. */
    nn_worker_task_feed (self, &stop);

    /*  Wait till worker thread terminates. */
    nn_thread_term (&self->thread);

    nn_list_term (&self->timeouts);
    brc = CloseHandle (self->cp);
    nn_assert_win (brc);
}

void nn_worker_fd_register (struct nn_worker *worker, nn_fd fd)
{
    HANDLE cp;

    cp = CreateIoCompletionPort ((HANDLE) fd, worker->cp, (ULONG_PTR) NULL, 0);
    nn_assert_win (cp == worker->cp);
}

static void nn_worker_routine (void *arg)
{
    OVERLAPPED_ENTRY entries [NN_WORKER_MAX_EVENTS];
    OVERLAPPED_ENTRY *entry;
    struct nn_timer *timer;
    struct nn_taskbase *task;
    struct nn_task_io *op;
    struct nn_worker *self;
    ULONG numentries;
    ULONG i;
    BOOL brc;
    int timeout;

    self = (struct nn_worker*) arg;

    while (1) {

        /*  Process all expired timers. */
        timeout = nn_worker_timer_process (self);

        /*  Wait for new events and/or timeouts. */
        brc = GetQueuedCompletionStatusEx (self->cp, entries,
            NN_WORKER_MAX_EVENTS, &numentries, timeout, FALSE);
        
        /*  A timer has expired; restart worker loop to process it. */
        if (!brc && GetLastError () == WAIT_TIMEOUT) {
            continue;
        }

        /*  One or more events has occurred. */
        nn_assert_win (brc);
        nn_assert_win (numentries > 0);

        for (i = 0; i != numentries; ++i) {

            entry = &entries [i];

            /*  Is this an I/O operation that has completed? */
            if (entry->lpOverlapped) {
                op = nn_cont (entry->lpOverlapped, struct nn_task_io, olpd);
                nn_assert (op->base.internaltype == NN_WORKER_TASK_IO);
                /*  The 'Internal' field is actually an NTSTATUS. Report
                    success and error. Ignore warnings and informational
                    messages. */
                switch (entry->Internal & 0xc0000000) {
                case 0x00000000:
                    op->result = 1;
                    break;
                case 0xc0000000:
                    op->result = 0;
                    break;
                default:
                    nn_assert_unreachable ("JRD - when?");
                    continue;
                }
                op->bytes = entry->dwNumberOfBytesTransferred;

                /*  Raise the completion event. */
                nn_ctx_enter (op->complete->dest->ctx);
                nn_assert (op->base.state == NN_STATE_TASK_ACTIVE);
                op->base.state = NN_STATE_TASK_DONE;
                nn_fsm_feed (op->complete->dest, op->complete->type, &op->base);
                nn_ctx_leave (op->complete->dest->ctx);

                continue;
            }

            task = (struct nn_taskbase *) entry->lpCompletionKey;
            switch (task->internaltype) {

            case NN_WORKER_TASK_STOP:
                /*  Worker thread shutdown is requested. */
                return;

            case NN_WORKER_TASK_IO:

            case NN_WORKER_TASK_ADDTIMER:
                /*  Timer added. */
                timer = nn_cont (task, struct nn_timer, base);
                nn_worker_timer_add (self, timer);
                continue;

            default:
                nn_assert_unreachable ("Unexpected [task->internaltype].");
                return;
            }

            nn_assert_unreachable ("Next iteration should have begun.");
        }
    }
}
#else

void nn_worker_fd_register (struct nn_worker *worker, nn_fd fd,
    struct nn_task_io *operation)
{
    nn_poller_add (&worker->poller, fd, &operation->hndl);
}

void nn_worker_rm_fd (struct nn_worker *self, struct nn_task_io *fd)
{
    nn_poller_rm (&self->poller, &fd->hndl);
}

void nn_worker_set_in (struct nn_worker *self, struct nn_task_io *fd)
{
    nn_poller_set_in (&self->poller, &fd->hndl);
}

void nn_worker_reset_in (struct nn_worker *self, struct nn_task_io *fd)
{
    nn_poller_reset_in (&self->poller, &fd->hndl);
}

void nn_worker_set_out (struct nn_worker *self, struct nn_task_io *fd)
{
    nn_poller_set_out (&self->poller, &fd->hndl);
}

void nn_worker_reset_out (struct nn_worker *self, struct nn_task_io *fd)
{
    nn_poller_reset_out (&self->poller, &fd->hndl);
}

void nn_worker_task_feed (struct nn_worker *self, const int *which)
{
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &task->item);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);
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
    nn_list_init (&self->timeouts);
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
    nn_list_term (&self->timeouts);
    nn_poller_term (&self->poller);
    nn_efd_term (&self->efd);
    nn_queue_item_term (&self->stop);
    nn_queue_term (&self->tasks);
    nn_mutex_term (&self->sync);
}

static void nn_worker_routine (void *arg)
{
    int rc;
    struct nn_worker *self;
    int pevent;
    struct nn_poller_hndl *phndl;
    struct nn_queue tasks;
    struct nn_queue_item *item;
    struct nn_worker_task *task;
    struct nn_task_io *io;
    struct nn_timer *timer;
    int timeout;

    self = (struct nn_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
        shut down. */
    while (1) {

        /*  Process all expired timers. */
        timeout = nn_worker_timer_process (self);

        /*  Wait for new events and/or timeouts. */
        rc = nn_poller_wait (&self->poller, timeout);
        errnum_assert (rc == 0, -rc);

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = nn_poller_event (&self->poller, &pevent, &phndl);
            if (rc == -EAGAIN)
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
                    if (!item)
                        break;

                    /*  If the worker thread is asked to stop, do so. */
                    if (item == &self->stop) {
                        /*  Remove and ignore all remaining tasks. */
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
            io = nn_cont (phndl, struct nn_task_io, hndl);
            nn_ctx_enter (io->owner->ctx);
            nn_fsm_feed (io->owner, io->src, pevent, io);
            nn_ctx_leave (io->owner->ctx);
        }
    }
}
#endif

struct nn_worker *nn_worker_choose (struct nn_fsm *fsm)
{
    return nn_pool_choose_worker (fsm->ctx->pool);
}

void nn_task_io_init (struct nn_task_io *io, struct nn_worker *worker, struct nn_fsm_event *complete)
{
    io->base.state = NN_STATE_TASK_IDLE;
    io->base.internaltype = NN_WORKER_TASK_IO;
    io->base.worker = worker;
    nn_queue_item_init (&io->base.item);
    io->complete = complete;
}

void nn_task_io_term (struct nn_task_io *io)
{
    nn_assert_state (&io->base, NN_STATE_TASK_IDLE);
    nn_queue_item_term (&io->base.item);
}

void nn_task_io_start (struct nn_task_io *io, int type)
{
    nn_assert_state (&io->base, NN_STATE_TASK_IDLE);
    io->base.state = NN_STATE_TASK_ACTIVE;
    io->complete->type = type;
    memset (&io->olpd, 0, sizeof (io->olpd));

    nn_worker_task_feed (io->base.worker, &io->base);
}

void nn_task_io_finish (struct nn_task_io *io, void *src)
{
    /*  Sanity check that the operation from the worker is the same that was
        initiated. */
    nn_assert (&io->base == src);
    nn_assert_state (&io->base, NN_STATE_TASK_DONE);
    io->base.state = NN_STATE_TASK_IDLE;
    io->result = 0;
    io->complete->type = 0;
    io->bytes = -1;
}

void nn_task_io_cancel (struct nn_task_io *io)
{
    nn_mutex_lock (&self->sync);
    nn_queue_remove (&self->tasks, &io->base.item);
    nn_mutex_unlock (&self->sync);
}

void nn_task_io_cancel (struct nn_task_io *io)
{
    BOOL brc;

    brc = CancelIoEx ((HANDLE) io->fd, &io->olpd);
//JRD - experimental
nn_assert (brc);
//if (!brc) {
//    err = GetLastError ();
//    nn_assert_win (err == ERROR_NOT_FOUND);
//}
}

void nn_timer_init (struct nn_timer *timer, struct nn_worker *worker, struct nn_fsm *owner)
{
    timer->base.state = NN_STATE_TASK_IDLE;
    timer->base.internaltype = NN_WORKER_TASK_ADDTIMER;
    timer->base.worker = worker;
    nn_queue_item_init (&timer->base.item);

    timer->complete.dest = owner;
    nn_list_item_init (&timer->item);
    timer->timeout = -1;
}

void nn_timer_term (struct nn_timer *timer)
{
    nn_assert_state (&timer->base, NN_STATE_TASK_IDLE);
    nn_queue_item_term (&timer->base.item);
    nn_list_item_term (&timer->item);
}

void nn_timer_start (struct nn_timer *timer, int type, int timeout)
{
    nn_assert_state (&timer->base, NN_STATE_TASK_IDLE);
    timer->base.state = NN_STATE_TASK_ACTIVE;

    /*  Negative timeout makes no sense. */
    nn_assert (timeout >= 0);

    timer->timeout = timeout;
    timer->complete.type = type;

    nn_worker_task_feed (timer->base.worker, &timer->base);
}

void nn_timer_cancel (struct nn_timer *timer)
{
    nn_list_erase (&timer->base.worker->timeouts, &timer->item);
}

int nn_timer_isidle (struct nn_timer *timer)
{
    return timer->base.state == NN_STATE_TASK_IDLE ? 1 : 0;
}
