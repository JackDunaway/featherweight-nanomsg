/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

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

/*  Stress test the IPC transport. */

/*  Test parameters. */
#define NUM_PRODUCERS 50
#define NUM_MSGS 50
#define SOCKET_ADDRESS "ipc://test-stress.ipc"

struct nn_sem ready;

struct stress_test_msg {
    int producer_id;
    int message_id;
};

static void consumer_routine (NN_UNUSED void *arg)
{
    struct stress_test_msg msg;
    int prev_msg [NUM_PRODUCERS] = {0};
    uint64_t stat;
    int timeo;
    int rc;
    int s;

    /*  Create consumer and notify launcher when ready. */
    s = test_socket (AF_SP, NN_PULL);
    test_bind (s, SOCKET_ADDRESS);
    timeo = 1000;
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo, sizeof (timeo));
    nn_sem_post (&ready);

    while (1)
    {
        /*  Initialize with invalid sentinel values. */
        msg.producer_id = -1;
        msg.message_id = -1;

        rc = nn_recv (s, &msg, sizeof (msg), 0);
        if (rc < 0) {
            nn_assert_is_error (rc == -1, ETIMEDOUT);
            break;
        }

        nn_assert (rc == sizeof (msg));

        /*  Sanity check for expected value range. */
        nn_assert (0 <= msg.producer_id && msg.producer_id < NUM_PRODUCERS);
        nn_assert (1 <= msg.message_id && msg.message_id <= NUM_MSGS);

        /*  Although ordering between producers is unconstrained, each
            individual producer should send in serial order. */
        nn_assert (prev_msg [msg.producer_id] + 1 == msg.message_id);

        prev_msg [msg.producer_id] = msg.message_id;
        nn_yield ();
    }

    /*  Final sanity checks on expected socket lifetime statistics. */
    stat = nn_get_statistic (s, NN_STAT_MESSAGES_RECEIVED);
    nn_assert (stat == NUM_PRODUCERS * NUM_MSGS);
    stat = nn_get_statistic (s, NN_STAT_BYTES_RECEIVED);
    nn_assert (stat == NUM_PRODUCERS * NUM_MSGS * sizeof (msg));

    /*  Clean up. */
    test_close (s);
}

static void producer_routine (void *arg)
{
    struct stress_test_msg msg;
    int time;
    int rc;
    int s;
    int i;

    /*  Store the serial ID of this producer and notify launcher we're ready. */
    msg.producer_id = *(int *) arg;
    nn_sem_post (&ready);

    /*  Stress test the lifecycle of sockets. */
    for (i = 1; i <= NUM_MSGS; i++) {
        s = test_socket (AF_SP, NN_PUSH);
        msg.message_id = i;
        test_connect (s, SOCKET_ADDRESS);
        time = test_wait_for_stat (s, NN_STAT_CURRENT_CONNECTIONS, 1, 5000);
        nn_assert (time >= 0);
        rc = nn_send (s, &msg, sizeof (msg), 0);
        nn_assert (rc == sizeof (msg));
        test_close (s);
        nn_yield ();
    }
}

int main (int argc, char *argv [])
{
    struct nn_thread producer [NUM_PRODUCERS];
    struct nn_thread consumer;
    int i;

    /*  Stress the shutdown algorithm. */
    nn_sem_init (&ready);
    nn_thread_init (&consumer, consumer_routine, NULL);
    nn_sem_wait (&ready);

    /*  Launch all producer threads. */
    for (i = 0; i != NUM_PRODUCERS; ++i) {
        nn_thread_init (&producer [i], producer_routine, &i);
        nn_sem_wait (&ready);
    }

    /*  Wait for all producers to complete. */
    for (i = 0; i != NUM_PRODUCERS; ++i) {
        nn_thread_term (&producer [i]);
    }

    /*  Wait for consumer thread to complete. */
    nn_thread_term (&consumer);
    nn_sem_term (&ready);

    return 0;
}
