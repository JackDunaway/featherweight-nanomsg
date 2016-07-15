/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
    Copyright (c) 2015-2016 Jack R. Dunaway. All rights reserved.

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
#define TEST_REPETITIONS 10
#define WORKERS 10
#define MSG_COUNT 100
#define MSG "PUB_SUB_STRESS"
#define MSGSZ (sizeof (MSG) - 1)

struct nn_test_args {
    struct nn_sem ready;
    char addr [128];
    int sock;
};

static void subscriber_worker (void *arg)
{
    struct nn_test_args self = {0};
    char msg [MSGSZ];
    int count = 0;
    uint64_t time;
    int rc;

    self = *(struct nn_test_args *) arg;

    test_setsockopt (self.sock, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);
    test_connect (self.sock, self.addr);
    time = test_wait_for_stat (self.sock, NN_STAT_CURRENT_CONNECTIONS, 1, 2000);
    nn_assert (time >= 0);

    nn_sem_post (&self.ready);

    while (1) {
        rc = nn_recv (self.sock, &msg, sizeof (msg), 0);
        if (rc != MSGSZ) {
            break;
        }
        count++;
        nn_yield ();
    }
    /*  Socket is expected to be closed by caller.  */
    nn_assert_is_error (rc == -1, EBADF);

    /*  Ensure at least one message was received, but no more than expected. */
    nn_assert (0 < count && count <= MSG_COUNT);
}

/*  Test condition of closing sockets that are blocking in another thread. */
void pubsub_test (int transport, int port)
{
    struct nn_thread pool [WORKERS];
    int sockets [WORKERS];
    struct nn_test_args args = {0};
    int timeo;
    int s;
    int j;

    /*  Initialize publisher socket. */
    test_get_transport_addr (args.addr, transport, port);
    s = test_socket (AF_SP, NN_PUB);
    test_bind (s, args.addr);
    timeo = 10;
    test_setsockopt (s, NN_SOL_SOCKET, NN_SNDTIMEO, &timeo, sizeof (timeo));
    nn_assert (timeo == 10);
    nn_sem_init (&args.ready);

    /*  Launch worker pool of subscribers. */
    for (j = 0; j < WORKERS; j++){
        sockets [j] = test_socket (AF_SP, NN_SUB);
        args.sock = sockets [j];
        nn_thread_init (&pool [j], subscriber_worker, &args);
        nn_sem_wait (&args.ready);
    }

    /*  Send some messages to ensure all workers in pool are active. */
    for (j = 0; j < MSG_COUNT; ++j) {
        test_send (s, MSG);
        nn_yield ();
    }

    /*  Wait for clean shutdown of worker pool. */
    for (j = 0; j < WORKERS; j++) {
        test_close (sockets [j]);
        nn_thread_term (&pool [j]);
    }

    /*  Clean up. */
    nn_sem_term (&args.ready);
    test_close (s);
}

int main (int argc, char *argv [])
{
    int i;

    int port = get_test_port (argc, argv);

    /*  Serially test each transport. */
    for (i = 0; i != TEST_REPETITIONS; ++i) {
        pubsub_test (NN_INPROC, port);
        pubsub_test (NN_IPC, port);
        pubsub_test (NN_TCP, port);
        pubsub_test (NN_WS, port);
    }
}
