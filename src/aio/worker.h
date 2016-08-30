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
#include "timerset.h"

#if defined NN_HAVE_WINDOWS

#include "../utils/win.h"
#include "../utils/thread.h"

#define NN_WORKER_OP_DONE 1
#define NN_WORKER_OP_ERROR 2

#define NN_WORKER_OP_STATE_IDLE 1
#define NN_WORKER_OP_STATE_ACCEPTING 2
#define NN_WORKER_OP_STATE_CONNECTING 3
#define NN_WORKER_OP_STATE_SENDING 4
#define NN_WORKER_OP_STATE_RECEIVING 5

#define NN_WORKER_TIMER_TIMEOUT 1

#define NN_WORKER_TASK_EXECUTE 1

struct nn_worker_timer {
    struct nn_fsm *owner;
    struct nn_timerset_hndl hndl;
};

struct nn_worker_task {
    int src;
    struct nn_fsm *owner;
};

struct nn_worker_op {
    int src;
    struct nn_fsm *owner;
    int state;
    size_t *pending_sz;
    size_t *pending_count;
    int success;

    /*  This structure is to be used by the user, not nn_worker_op itself.
        Actual usage is specific to the asynchronous operation in question. */
    OVERLAPPED olpd;
};

struct nn_worker {
    HANDLE cp;
    struct nn_timerset timerset;
    struct nn_thread thread;
};

struct nn_worker *nn_worker_choose (struct nn_fsm *fsm);

void nn_worker_op_init (struct nn_worker_op *self, int src,
    struct nn_fsm *owner);
void nn_worker_op_term (struct nn_worker_op *self);

/*  Call this function when asynchronous operation is started. */
void nn_worker_op_start (struct nn_worker_op *self, int state);

int nn_worker_op_isidle (struct nn_worker_op *self);

void nn_worker_register_iocp (struct nn_fsm *fsm, HANDLE h);

#else

#include "../utils/queue.h"
#include "../utils/mutex.h"
#include "../utils/thread.h"
#include "../utils/efd.h"

#include "poller.h"

#define NN_WORKER_FD_IN NN_POLLER_IN
#define NN_WORKER_FD_OUT NN_POLLER_OUT
#define NN_WORKER_FD_ERR NN_POLLER_ERR

struct nn_worker_fd {
    int src;
    struct nn_fsm *owner;
    struct nn_poller_hndl hndl;
};

struct nn_worker_task {
    int src;
    struct nn_fsm *owner;
    struct nn_queue_item item;
};

struct nn_worker {
    struct nn_mutex sync;
    struct nn_queue tasks;
    struct nn_queue_item stop;
    struct nn_efd efd;
    struct nn_poller poller;
    struct nn_poller_hndl efd_hndl;
    struct nn_timerset timerset;
    struct nn_thread thread;
};

void nn_worker_fd_init (struct nn_worker_fd *self, int src,
    struct nn_fsm *owner);
void nn_worker_fd_term (struct nn_worker_fd *self);

void nn_worker_add_fd (struct nn_worker *self, int s, struct nn_worker_fd *fd);
void nn_worker_rm_fd(struct nn_worker *self, struct nn_worker_fd *fd);
void nn_worker_set_in (struct nn_worker *self, struct nn_worker_fd *fd);
void nn_worker_reset_in (struct nn_worker *self, struct nn_worker_fd *fd);
void nn_worker_set_out (struct nn_worker *self, struct nn_worker_fd *fd);
void nn_worker_reset_out (struct nn_worker *self, struct nn_worker_fd *fd);
#endif

void nn_worker_timer_init (struct nn_worker_timer *self, struct nn_fsm *owner);
void nn_worker_timer_term (struct nn_worker_timer *self);
int nn_worker_timer_isactive (struct nn_worker_timer *self);

void nn_worker_task_init (struct nn_worker_task *self, int src,
    struct nn_fsm *owner);
void nn_worker_task_term (struct nn_worker_task *self);

int nn_worker_init (struct nn_worker *self);
void nn_worker_term (struct nn_worker *self);
void nn_worker_execute (struct nn_worker *self, struct nn_worker_task *task);
void nn_worker_cancel (struct nn_worker *self, struct nn_worker_task *task);

void nn_worker_add_timer (struct nn_worker *self, int timeout,
    struct nn_worker_timer *timer);
void nn_worker_rm_timer (struct nn_worker *self,
    struct nn_worker_timer *timer);

#endif

