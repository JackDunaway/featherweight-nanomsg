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

#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>

#include "testutil.h"
#include "../src/utils/atomic.c"
#include "../src/utils/condvar.c"

/*  Test parameters. */
#define NUM_IMPATIENT_REQS 50
#define NUM_MSGS 20

char addr [128];
struct nn_sem ready;
struct nn_atomic num_reqs_active;

struct impatient_test_msg {
    int req_id;
    int msg_id;
    char payload [8];
};

#define MSG_SZ (sizeof (struct impatient_test_msg))

static void impatient_req_routine (void *arg)
{
    struct impatient_test_msg msg;
    struct impatient_test_msg reply;
    int bufsz;
    int timeo;
    int time;
    int rc;
    int s;

    /*  Create impatient REQ and wait until connected. */
    msg.req_id = *(int *) arg;
    s = test_socket (AF_SP, NN_REQ);

    bufsz = 0;
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVBUF, &bufsz, sizeof (bufsz));
    bufsz = 0;
    test_setsockopt (s, NN_SOL_SOCKET, NN_SNDBUF, &bufsz, sizeof (bufsz));
    
    /*  Intentionally set an unreasonably short timeout for the nominal,
        expected network operating conditions. */
    timeo = 1;
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));

    /*  Wait until connected to put as much stress on REP as possible. */
    test_connect (s, addr);
    time = test_wait_for_stat (s, NN_STAT_CURRENT_CONNECTIONS, 1, 5000);
    nn_assert (time >= 0);

    nn_atomic_inc (&num_reqs_active, 1);
    nn_sem_post (&ready);

    /*  Keep impatiently sending requests. */
    for (msg.msg_id = 0; msg.msg_id < NUM_MSGS; ++msg.msg_id) {
        
        /*  A full REQ socket effectively always succeeds in its request. */
        rc = nn_send (s, &msg, MSG_SZ, NN_DONTWAIT);
        nn_assert (rc == MSG_SZ);

        rc = nn_recv (s, &reply, MSG_SZ, 0);

        /*  Success is the less likely case ... */
        if (rc == MSG_SZ) {
            nn_assert (msg.msg_id == reply.msg_id && msg.req_id == reply.req_id);
            continue;
        }

        /*  ... and failure is the more likely case. */
        nn_assert_is_error (rc == -1, ETIMEDOUT);
    }

    nn_atomic_dec (&num_reqs_active, 1);
    test_close (s);
}

static void overworked_rep_routine (int transport, int port)
{
    struct nn_thread req_pool [NUM_IMPATIENT_REQS];
    int msg_id [NUM_IMPATIENT_REQS];
    struct impatient_test_msg request;
    int bufsz;
    int timeo;
    int rep;
    int rc;
    int i;

    test_get_transport_addr (addr, transport, port);

    /*  Create the overworked REP socket. */
    nn_sem_init (&ready);
    nn_atomic_init (&num_reqs_active, 0);
    rep = test_socket (AF_SP, NN_REP);

    /*  Ensure the REP socket freewheels at the end of the test to continue
        checking whether REQ pool is still active. */
    timeo = 1;
    test_setsockopt (rep, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));

    bufsz = 0;
    test_setsockopt (rep, NN_SOL_SOCKET, NN_RCVBUF, &bufsz, sizeof (bufsz));
    bufsz = 0;
    test_setsockopt (rep, NN_SOL_SOCKET, NN_SNDBUF, &bufsz, sizeof (bufsz));

    test_bind (rep, addr);

    /*  Launch impatient REQ socket pool. */
    for (i = 0; i != NUM_IMPATIENT_REQS; ++i) {
        nn_thread_init (&req_pool [i], impatient_req_routine, &i);
        nn_sem_wait (&ready);
        msg_id [i] = -1;
    }

    /*  Service requests til all impatient REQs have left. */
    while (num_reqs_active.n) {

        /*  Establish precondition to ensure postcondition is not stale. */
        request.msg_id = -1;
        request.req_id = -1;

        rc = nn_recv (rep, &request, MSG_SZ, 0);
        if (rc < 0) {
            nn_assert_is_error (rc == -1, ETIMEDOUT);
            nn_assert (request.msg_id == -1);
            nn_assert (request.req_id == -1);
            continue;
        }

        /*  Perform basic sanity checks on received message. */
        nn_assert (rc == MSG_SZ);
        nn_assert (0 <= request.req_id && request.req_id < NUM_IMPATIENT_REQS);

        /*  Ensure each subsequent message from each client is in order. */
        nn_assert (msg_id [request.req_id] < request.msg_id);
        msg_id [request.req_id] = request.msg_id;

        /*  Simulate a non-trivial, predictable amount of work performed by
            server prior to sending a response. */
        nn_sleep (1);

        /*  A full REP socket effectively always succeeds on reply. */
        rc = nn_send (rep, &request, MSG_SZ, NN_DONTWAIT);
        nn_assert (rc == MSG_SZ);
    }

    /*  Clean up. */
    for (i = 0; i != NUM_IMPATIENT_REQS; ++i) {
        nn_thread_term (&req_pool [i]);
    }

    test_close (rep);
    nn_atomic_term (&num_reqs_active);
    nn_sem_term (&ready);
}

int main (int argc, char *argv [])
{
    int port = get_test_port (argc, argv);

    /*  Simulate a pool of impatient REQ sockets that give up and retry before
        a reasonable amount of time has been given to an overworked REP. This test
        is to ensure the entire system remains stable during such flood of
        abandoned requests. */

    overworked_rep_routine (NN_INPROC, port);
    overworked_rep_routine (NN_IPC, port);
    overworked_rep_routine (NN_TCP, port);
    overworked_rep_routine (NN_WS, port);

    _CrtDumpMemoryLeaks();

    return 0;
}
