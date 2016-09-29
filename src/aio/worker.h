/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#ifndef NN_WORKER_INCLUDED
#define NN_WORKER_INCLUDED

#include "fsm.h"
#include "poller.h"

#include "../utils/clock.h"
#include "../utils/efd.h"
#include "../utils/list.h"
#include "../utils/mutex.h"
#include "../utils/queue.h"
#include "../utils/thread.h"

/*  Possible states of a worker task. */
#define NN_STATE_TASK_IDLE 1
#define NN_STATE_TASK_ACTIVE 2
#define NN_STATE_TASK_DONE 3

/*  Events emitted by a TIMER object. */
#define NN_EVENT_TIMER_STOPPED 0x00420000

/*  Base class for all tasks. A task represents one finite operation fed to
    the worker, which will then report back the result asynchronously. */
struct nn_taskbase {

    /*  The worker on which the task is running. This is cached within each
        task so that the object listening for the event competion notification
        can cancel the task while remaining abstracted from where the task
        is actually executing. */
    struct nn_worker *worker;

    /*  Current state of the task. */
    int state;

    /*  Task type that instructs the worker how to handle the task. */
    int internaltype;

    /*  Internal structure used for task bookkeeping by the worker. */
    struct nn_queue_item item;
};

/*  Subclass of nn_taskbase. Instructs the worker to timeout after a period
    and send notification once the timer expires. */
struct nn_timer {

    /*  Superclass base object. */
    struct nn_taskbase base;

    /*  Notification sent to task owner once operation is complete. */
    struct nn_fsm_event complete;

    /*  Number of milliseconds to timeout after timer starts. */
    int timeout;

    /*  Clock value in the future calculated the moment the timer starts, past
        which point the complete event will fire. */
    uint64_t expiry;

    /*  Internal structure used for task bookkeeping by the worker. */
    struct nn_list_item item;
};

/*  Subclass of nn_taskbase. Instructs the worker to perform an async I/O
    operation then report back a notification once finished. The exact
    implementation is platform-specific, declared here as a placeholder. */
//struct nn_task_io;

#if defined NN_HAVE_WINDOWS

#include "../utils/win.h"

#define NN_INFINITE_TIMEOUT INFINITE

struct nn_task_io {

    /*  Superclass base object. */
    struct nn_taskbase base;

    /*  Notification sent to task owner once operation is complete. */
    struct nn_fsm_event *complete;

    /*  Status code indicating the result of the async operation. */
    int result;

    /*  Bytes transferred by the async operation. */
    DWORD bytes;

    /*  Opaque structure used only by the Worker. */
    OVERLAPPED olpd;
};

struct nn_worker {

    /*  IO Completion Port (IOCP) whose events are serviced by this worker. */
    HANDLE cp;

    /*  List of currently-active timers on this worker. */
    struct nn_list timeouts;

    /*  Worker thread. */
    struct nn_thread thread;
};

#else

#define NN_INFINITE_TIMEOUT -1

struct nn_task_io {
    struct nn_taskbase task;
    struct nn_poller_hndl hndl;
};

struct nn_worker {
    struct nn_mutex sync;
    struct nn_queue tasks;
    struct nn_queue_item stop;
    struct nn_efd efd;
    struct nn_poller poller;
    struct nn_poller_hndl efd_hndl;
    struct nn_list timeouts;
    struct nn_thread thread;
};
#endif

int nn_worker_init (struct nn_worker *self);
void nn_worker_term (struct nn_worker *self);

struct nn_worker *nn_worker_choose (struct nn_fsm *fsm);

void nn_worker_fd_register (struct nn_worker *worker, nn_fd fd);
void nn_worker_fd_unregister (struct nn_worker *worker, nn_fd fd);
void nn_worker_rm_fd (struct nn_worker *worker, struct nn_task_io *io);
void nn_worker_set_in (struct nn_worker *worker, struct nn_task_io *io);
void nn_worker_reset_in (struct nn_worker *worker, struct nn_task_io *io);
void nn_worker_set_out (struct nn_worker *worker, struct nn_task_io *io);
void nn_worker_reset_out (struct nn_worker *worker, struct nn_task_io *io);

void nn_task_io_init (struct nn_task_io *io, struct nn_worker *worker, struct nn_fsm_event *complete);
void nn_task_io_term (struct nn_task_io *io);
void nn_task_io_start (struct nn_task_io *io, int task);
void nn_task_io_cancel (struct nn_task_io *io);
void nn_task_io_finish (struct nn_task_io *io, void *src);

void nn_timer_init (struct nn_timer *timer, struct nn_worker *worker, struct nn_fsm *owner);
void nn_timer_term (struct nn_timer *timer);
void nn_timer_start (struct nn_timer *timer, int type, int timeout);
void nn_timer_cancel (struct nn_timer *timer);
int nn_timer_isidle (struct nn_timer *timer);


#endif

