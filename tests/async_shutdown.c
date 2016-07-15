/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2015-2016 Jack R. Dunaway.  All rights reserved.
    Copyright 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

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

#include "testutil.h"

/*  Test parameters. */
#define WORKERS 10
struct nn_sem ready;

static void blocking_io_worker (void *arg)
{
    int type;
    int msg;
    int rc;
    int s;

    s = *((int *) arg);

    /*  This socket type will determine the method chosen to begin a blocking
        I/O call. */
    type = test_get_socket_sp (s);

    /*  Start a blocking I/O operation which we expect to fail. For this
        reason, the datatype and contents of 'msg' is irrelevant. */
    switch (type) {

    case NN_PAIR:
    case NN_REP:
    case NN_SUB:
    case NN_RESPONDENT:
    case NN_PULL:
    case NN_BUS:
        /*  All socket topologies that can immediately block with a recv. */
        nn_sem_post (&ready);
        nn_clear_errno ();
        msg = -42;
        rc = nn_recv (s, &msg, sizeof (msg), 0);
        nn_assert_is_error (rc == -1 && msg == -42, EBADF);
        break;

    case NN_PUSH:
        /*  All socket topologies that can immediately block with a send. */
        nn_sem_post (&ready);
        nn_clear_errno ();
        msg = -42;
        rc = nn_send (s, &msg, sizeof (msg), 0);
        nn_assert_is_error (rc == -1 && msg == -42, EBADF);
        break;

    case NN_REQ:
    case NN_SURVEYOR:
        /*  Abiding by protocol, send first then wait in a blocking recv. */
        msg = -42;
        rc = nn_send (s, &msg, sizeof (msg), 0);
        nn_assert (rc == sizeof (msg));
        nn_sem_post (&ready);
        nn_clear_errno ();
        rc = nn_recv (s, &msg, sizeof (msg), 0);
        nn_assert_is_error (rc == -1 && msg == -42, EBADF);
        break;

    case NN_PUB:
        nn_assert_unreachable ("NN_PUB doesn't block.");
        break;

    default:
        nn_assert_unreachable ("Unexpected socket type.");
        break;
    }
}

/*  Test condition of closing a socket while it currently has many blocking
    (or just preparing to block) I/O callsites in another thread. */
void many_callsites_blocking_test (int transport, int port)
{
    struct nn_thread pool [WORKERS];
    char addr [128];
    int j;
    int s;

    /*  Initialize local socket. */
    test_get_transport_addr (addr, transport, port);
    s = test_socket (AF_SP, transport);
    test_bind (s, addr);
    nn_sem_init (&ready);
    for (j = 0; j != WORKERS; ++j) {
        nn_thread_init (&pool [j], blocking_io_worker, &s);
        nn_sem_wait (&ready);
    }

    /*  With all callsites now blocking, close. */
    test_close (s);

    /*  Expect a clean shutdown from all workers. */
    for (j = 0; j != WORKERS; ++j) {
        nn_thread_term (&pool [j]);
    }

    /*  Clean up. */
    nn_sem_term (&ready);
}

/*  Test condition of closing a socket while it is currently blocking
    (or just preparing to block) in another thread. */
void one_callsite_blocking_test (int protocol, int transport, int port)
{
    struct nn_thread blocker;
    char addr [128];
    int j;
    int s;

    nn_sem_init (&ready);
    for (j = 0; j != WORKERS; ++j) {

        /*  Initialize local socket. */
        test_get_transport_addr (addr, transport, port);
        s = test_socket (AF_SP, protocol);
        test_bind (s, addr);

        /*  Share socket with an async thread. */
        nn_thread_init (&blocker, blocking_io_worker, &s);
        
        /*  Wait for it to block, then close. */
        nn_sem_wait (&ready);
        nn_sleep (5);
        test_close (s);
        nn_thread_term (&blocker);
    }
    nn_sem_term (&ready);
}

int main (int argc, char *argv [])
{
    int transport;
    int i;

    int port = get_test_port (argc, argv);

    /*  Test shutdown behavior for all SP topologies (except for NN_PUB, which
        by design effectively does not block). */
    for (i = 0; i != NN_TEST_ALL_TRANSPORTS_LEN; ++i) {

        transport = NN_TEST_ALL_TRANSPORTS [i];

        one_callsite_blocking_test (NN_PAIR, transport, port);
        one_callsite_blocking_test (NN_REQ, transport, port);
        one_callsite_blocking_test (NN_REP, transport, port);
        one_callsite_blocking_test (NN_SUB, transport, port);
        one_callsite_blocking_test (NN_SURVEYOR, transport, port);
        one_callsite_blocking_test (NN_RESPONDENT, transport, port);
        one_callsite_blocking_test (NN_PUSH, transport, port);
        one_callsite_blocking_test (NN_PULL, transport, port);
        one_callsite_blocking_test (NN_BUS, transport, port);

        one_callsite_blocking_test (NN_PAIR, transport, port);
        one_callsite_blocking_test (NN_REQ, transport, port);
        one_callsite_blocking_test (NN_REP, transport, port);
        one_callsite_blocking_test (NN_SUB, transport, port);
        one_callsite_blocking_test (NN_SURVEYOR, transport, port);
        one_callsite_blocking_test (NN_RESPONDENT, transport, port);
        one_callsite_blocking_test (NN_PUSH, transport, port);
        one_callsite_blocking_test (NN_PULL, transport, port);
        one_callsite_blocking_test (NN_BUS, transport, port);
    }

    return 0;
}
