/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
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
#define MESSAGES_PER_WORKER 10
#define TEST_REPETITIONS 10
#define MSG "hello"
#define MSGSZ (sizeof (MSG) - 1)

struct nn_test_race_args {
    struct nn_sem ready;
    char addr [128];
};


/*  Immediately close socket after beginning the async connection process,
    intentionally creating a race whether or not the connection is able to
    establish prior to shutdown. */
static void shutdown_during_connect_routine (void *args)
{
    char *addr = args;
    int s;

    s = test_socket (AF_SP, NN_SUB);
    test_connect (s, addr);
    test_close (s);
}

/*  Test clean shutdown of many concurrent sockets waiting to connect. */
void shutdown_during_connect (int transport, int port)
{
    struct nn_thread pool [WORKERS];
    char addr [128];
    int s;
    int i;

    /*  Initialize publisher socket. */
    test_get_transport_addr (addr, transport, port);
    s = test_socket (AF_SP, NN_PUB);
    test_bind (s, addr);

    /*  Launch worker pool of subscribers. */
    for (i = 0; i != WORKERS; ++i) {
        nn_thread_init (&pool [i], shutdown_during_connect_routine, addr);
    }

    /*  Wait for clean shutdown of worker pool. */
    for (i = 0; i != WORKERS; ++i) {
        nn_thread_term (&pool [i]);
    }

    /*  Clean up. */
    test_close (s);
}

static void shutdown_race_routine (void *args)
{
    struct nn_test_race_args *self = (struct nn_test_race_args *) args;
    int ms;
    int s;
    int i;

    /*  Initialize consumer. */
    s = test_socket (AF_SP, NN_PULL);
    ms = 1000;
    test_setsockopt (s, NN_SOL_SOCKET, NN_RCVTIMEO, &ms, sizeof (ms));
    test_connect (s, self->addr);
    nn_sem_post (&self->ready);

    /*  Receive a finite number of messages. */
    for (i = 0; i != MESSAGES_PER_WORKER; ++i) {
        nn_yield ();
        test_recv (s, MSG);
    }

    /*  Shut down while the producer continues to send. */
    test_close (s);
}

/*  Test race condition of sending message while socket shutting down. */
void shutdown_race_test (int transport, int port)
{
    struct nn_thread pool [WORKERS];
    struct nn_test_race_args args = {0};
    int timeo;
    int count;
    int rc;
    int s;
    int j;

    /*  Initialize producer socket. */
    test_get_transport_addr (args.addr, transport, port);
    s = test_socket (AF_SP, NN_PUSH);
    test_bind (s, args.addr);
    timeo = 10;
    test_setsockopt (s, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_sem_init (&args.ready);

    /*  Launch worker pool of consumers. */
    for (j = 0; j != WORKERS; ++j) {
        nn_thread_init (&pool [j], shutdown_race_routine, &args);
        nn_sem_wait (&args.ready);
    }

    /*  Loop until the first timeout indicating all workers are gone. */
    count = 0;
    while (1) {
        nn_clear_errno ();
        rc = nn_send (s, MSG, MSGSZ, 0);
        if (rc == MSGSZ) {
            nn_yield ();
            count++;
            continue;
        }
        nn_assert_is_error (rc == -1, ETIMEDOUT);
        break;
    }

    /*  Once all workers are gone, ensure that the total number of messages
        sent is at least the total expected workload. */
    nn_assert (count >= MESSAGES_PER_WORKER * WORKERS);
    nn_assert_stat_value (s, NN_STAT_CURRENT_CONNECTIONS, 0);
    nn_assert_stat_value (s, NN_STAT_BYTES_SENT, count * MSGSZ);

    /*  Wait for clean shutdown of worker pool. */
    for (j = 0; j != WORKERS; ++j) {
        nn_thread_term (&pool [j]);
    }

    /*  Clean up. */
    nn_sem_term (&args.ready);
    test_close (s);
}

static void shutdown_immediate_routine (void *args)
{
    /*  No soup for you. Come back. One year. */
    test_close (*(int *) args);
}

/*  Create a bunch of sockets and shut them down.  */
void shutdown_immediate_test (int transport, int protocol)
{
    struct nn_thread pool [WORKERS * 2];
    int socket [WORKERS * 2];
    int j;

    /*  Create a lot of sockets and share them with a worker. */
    for (j = 0; j != WORKERS; ++j) {

        /*  Full socket.... */
        socket [j] = test_socket (AF_SP, protocol);
        nn_thread_init (&pool [j], shutdown_immediate_routine, &socket [j]);

        /*  ... and raw socket, too. */
        socket [j + WORKERS] = test_socket (AF_SP_RAW, protocol);
        nn_thread_init (&pool [j + WORKERS], shutdown_immediate_routine,
            &socket [j + WORKERS]);
    }

    /*  Wait for clean shutdown of worker pool. */
    for (j = 0; j != WORKERS; ++j) {
        nn_thread_term (&pool [j]);
        nn_thread_term (&pool [j + WORKERS]);
    }

}

int main (int argc, char *argv [])
{
    int port = get_test_port (argc, argv);
    int protocol;
    int i;
    int j;

    /*  Test immediate, async shutdown behavior for all SP topologies
        across all transports. */
    for (j = 0; j != NN_TEST_ALL_SP_LEN; ++j) {
        protocol = NN_TEST_ALL_SP [j];
        shutdown_immediate_test (NN_INPROC, protocol);
        shutdown_immediate_test (NN_IPC, protocol);
        shutdown_immediate_test (NN_TCP, protocol);
        shutdown_immediate_test (NN_WS, protocol);
    }

    /*  Stress test shutdown behavior for all transport types. */
    for (i = 0; i != TEST_REPETITIONS; ++i) {

        shutdown_during_connect (NN_INPROC, port);
        shutdown_during_connect (NN_IPC, port);
        shutdown_during_connect (NN_TCP, port);
        shutdown_during_connect (NN_WS, port);

        /*  TODO: this can deadlock due `nn_fsm_raiseto()` */
        shutdown_race_test (NN_INPROC, port);
        shutdown_race_test (NN_IPC, port);
        shutdown_race_test (NN_TCP, port);
        shutdown_race_test (NN_WS, port);
    }

    return 0;
}
