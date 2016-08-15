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

#include "testutil.h"
#include "../src/utils/atomic.c"
#include "../src/utils/condvar.c"

/*  Test parameters. */
#define NUM_IMPATIENT_REQS 1
#define BACKOFF_INCREMENT_MSEC 0
#define MSG_GOAL 10

char addr [128];
struct nn_atomic num_reqs_active;
struct nn_atomic rep_active;
struct nn_sem ready;

struct impatient_test_msg {
    int req_id;
    int msg_id;
    char payload [9992];
    //char payload [99992];
};

#define MSG_SZ (sizeof (struct impatient_test_msg))

int msg_id [NUM_IMPATIENT_REQS];

static void impatient_req_routine (void *arg)
{
    struct impatient_test_msg msg;
    struct impatient_test_msg reply;
    int backoff_timeo;
    int successes;
    int bufsz;
    int time;
    int rc;
    int s;

    /*  Create impatient REQ and wait until connected. */
    msg.req_id = *(int *) arg;
    s = test_socket (AF_SP, NN_REQ);

    //bufsz = 0;
    //test_setsockopt (s, NN_SOL_SOCKET, NN_RCVBUF,
    //    &bufsz, sizeof (bufsz));
    //bufsz = 0;
    //test_setsockopt (s, NN_SOL_SOCKET, NN_SNDBUF,
    //    &bufsz, sizeof (bufsz));


    test_connect (s, addr);
    time = test_wait_for_stat (s, NN_STAT_CURRENT_CONNECTIONS, 1, 5000);
    nn_assert (time >= 0);
    nn_atomic_inc (&num_reqs_active, 1);
    nn_sem_post (&ready);

    backoff_timeo = 10;
    successes = 0;
    msg.msg_id = 0;

    /*  Keep impatiently sending messages until finally meeting goal of
        successful message transactions. */
    while (successes < MSG_GOAL && rep_active.n) {

        /*  Intentionally set an unreasonably short timeout for the nominal,
            expected network operating conditions, yet slowly back off to
            ensure the test can eventually finish. */
        if (msg.msg_id % MSG_GOAL == 0) {
            test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO,
                &backoff_timeo, sizeof (backoff_timeo));
            test_setsockopt (s, NN_SOL_SOCKET, NN_SNDTIMEO,
                &backoff_timeo, sizeof (backoff_timeo));
            backoff_timeo += BACKOFF_INCREMENT_MSEC;
        }
        ++msg.msg_id;

        if (msg.req_id == 0)
            printf ("%04d        %04d\n", msg.msg_id, msg.msg_id - msg_id [msg.req_id]);
        //rc = nn_send (s, &msg, MSG_SZ, NN_DONTWAIT);
        rc = nn_send (s, &msg, MSG_SZ, 0);
        nn_assert (rc == MSG_SZ);

        rc = nn_recv (s, &reply, MSG_SZ, 0);

        /*  Success is the less likely case ... */
        if (rc == MSG_SZ) {
            nn_assert (msg.msg_id == reply.msg_id && msg.req_id == reply.req_id);
            ++successes;
            continue;
        }

        /*  ... and failure is the more likely case. */
        nn_assert_is_error (rc == -1, ETIMEDOUT);
    }

    printf ("%05d messages sent\n", msg.msg_id);

    nn_atomic_dec (&num_reqs_active, 1);
    test_close (s);
}

int main (int argc, char *argv [])
{
    struct nn_thread req_pool [NUM_IMPATIENT_REQS];
    struct impatient_test_msg request;
    int bufsz;
    int port;
    int rep;
    int rc;
    int i;

    /*  Simulate a pool of impatient REQ sockets that give up and retry before
        a reasonable amount of time has been given to an overworked REP. This test
        is to ensure the entire system remains stable during such flood of
        abandoned requests. */

    port = get_test_port (argc, argv);
    test_get_transport_addr (addr, NN_TCP, port);

    /*  Create the overworked REP socket. */
    nn_sem_init (&ready);
    nn_atomic_init (&rep_active, 1);
    nn_atomic_init (&num_reqs_active, 0);
    rep = test_socket (AF_SP, NN_REP);



    bufsz = 100;
    test_setsockopt (rep, NN_SOL_SOCKET, NN_RCVBUF,
        &bufsz, sizeof (bufsz));
    bufsz = 100;
    test_setsockopt (rep, NN_SOL_SOCKET, NN_SNDBUF,
        &bufsz, sizeof (bufsz));


    test_bind (rep, addr);

    /*  Launch impatient REQ socket pool. */
    for (i = 0; i != NUM_IMPATIENT_REQS; ++i) {
        nn_thread_init (&req_pool [i], impatient_req_routine, &i);
        nn_sem_wait (&ready);
        msg_id [i] = -1;
    }
    nn_sleep (30000);

    /*  Service requests til all impatient REQs have been satisfied. */
    while (num_reqs_active.n) {

        /*  Establish precondition to ensure postcondition is not stale. */
        request.msg_id = -1;
        request.req_id = -1;

        rc = nn_recv (rep, &request, MSG_SZ, NN_DONTWAIT);
        if (rc < 0) {
            nn_assert_is_error (rc == -1, EAGAIN);
            nn_assert (request.msg_id == -1);
            nn_assert (request.req_id == -1);
            continue;
        }

        nn_assert (rc == MSG_SZ);
        nn_assert (0 <= request.req_id && request.req_id < NUM_IMPATIENT_REQS);

        /*  Ensure each subsequent message from each client is in order. */
        nn_assert (msg_id [request.req_id] < request.msg_id);
        msg_id [request.req_id] = request.msg_id;

        /*  Simulate a non-trivial, predictable amount of work performed by
            server prior to sending a response. */
        nn_sleep (BACKOFF_INCREMENT_MSEC);

        rc = nn_send (rep, &request, MSG_SZ, NN_DONTWAIT);
        nn_assert (rc == MSG_SZ);
    }

    /*  Clean up. */
    nn_atomic_dec (&rep_active, 1);
    for (i = 0; i != NUM_IMPATIENT_REQS; ++i) {
        nn_thread_term (&req_pool [i]);
    }

    test_close (rep);
    nn_atomic_term (&num_reqs_active);
    nn_atomic_term (&rep_active);
    nn_sem_term (&ready);

    return 0;
}
